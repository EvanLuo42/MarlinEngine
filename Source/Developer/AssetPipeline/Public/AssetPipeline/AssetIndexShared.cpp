#include "AssetPipeline/AssetIndexShared.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
constexpr uint32 ControlMagic = 0x58444941u;
constexpr uint32 ControlVersion = 1;
constexpr usize MaxPath = 1024;
constexpr usize KeptSegments = 3;

/// The control block. Sequence is odd while the publisher writes; readers retry until they see the same even value
/// before and after copying the rest.
struct Control
{
    uint32 Magic;
    uint32 Version;
    Atomic<uint64> Sequence;
    uint64 Generation;
    uint64 DeltaSize;
    uint64 BaseSize;
    char BasePath[MaxPath];
};

struct Published
{
    uint64 Generation = 0;
    uint64 DeltaSize = 0;
    uint64 BaseSize = 0;
    char BasePath[MaxPath] = {};
};

String ControlName(StringView name)
{
    return Format("{0}.Index", name);
}

String SegmentName(StringView name, uint64 generation)
{
    return Format("{0}.Index.{1}", name, generation);
}

Control &ControlOf(const SharedMemory &memory)
{
    return *static_cast<Control *>(memory.Data);
}

bool ReadPublished(const Control &control, Published &published)
{
    for (int32 attempt = 0; attempt < 1000; ++attempt)
    {
        const uint64 before = control.Sequence.Load(MemoryOrder::Acquire);
        if ((before & 1) != 0)
        {
            continue;
        }
        published.Generation = control.Generation;
        published.DeltaSize = control.DeltaSize;
        published.BaseSize = control.BaseSize;
        MemCopy(published.BasePath, control.BasePath, MaxPath);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (control.Sequence.Load(MemoryOrder::Relaxed) == before)
        {
            published.BasePath[MaxPath - 1] = '\0';
            return before != 0;
        }
    }
    return false;
}
} // namespace

AssetIndexPublisher::~AssetIndexPublisher()
{
    Close();
}

bool AssetIndexPublisher::Open(StringView name)
{
    Close();
    _name = name;
    if (!PlatformSharedMemory::Create(ControlName(name).CStr(), sizeof(Control), _control))
    {
        return false;
    }
    Control &control = ControlOf(_control);
    control.Magic = ControlMagic;
    control.Version = ControlVersion;
    return true;
}

void AssetIndexPublisher::Close()
{
    for (Segment &segment : _segments)
    {
        PlatformSharedMemory::Close(segment.Memory);
    }
    _segments.Clear();
    if (_control.Data != nullptr)
    {
        ControlOf(_control).Sequence.Store(0, MemoryOrder::Release);
        PlatformSharedMemory::Close(_control);
    }
    _published = ~uint64{0};
}

bool AssetIndexPublisher::Publish(const AssetIndex &index)
{
    if (_control.Data == nullptr)
    {
        return false;
    }
    bool ok = false;
    index.WithImages([&](Span<const uint8> base, Span<const uint8> delta, uint64 generation, StringView basePath) {
        if (generation == _published || basePath.IsEmpty() || basePath.Length() >= MaxPath)
        {
            ok = generation == _published;
            return;
        }
        Segment segment;
        segment.Generation = generation;
        const usize size = delta.Count() != 0 ? delta.Count() : 1;
        if (!PlatformSharedMemory::Create(SegmentName(_name.AsView(), generation).CStr(), size, segment.Memory))
        {
            return;
        }
        MemCopy(segment.Memory.Data, delta.Data(), delta.Count());
        Control &control = ControlOf(_control);
        const uint64 sequence = control.Sequence.Load(MemoryOrder::Relaxed);
        control.Sequence.Store(sequence + 1, MemoryOrder::Relaxed);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        control.Generation = generation;
        control.DeltaSize = delta.Count();
        control.BaseSize = base.Count();
        MemZero(control.BasePath, MaxPath);
        MemCopy(control.BasePath, basePath.Data(), basePath.Length());
        control.Sequence.Store(sequence + 2, MemoryOrder::Release);
        _segments.Add(segment);
        while (_segments.Count() > KeptSegments)
        {
            PlatformSharedMemory::Close(_segments[0].Memory);
            _segments.RemoveAt(0);
        }
        _published = generation;
        ok = true;
    });
    return ok;
}

AssetIndexReader::~AssetIndexReader()
{
    Close();
}

bool AssetIndexReader::Open(StringView name)
{
    Close();
    _name = name;
    if (!PlatformSharedMemory::Open(ControlName(name).CStr(), sizeof(Control), _control))
    {
        return false;
    }
    if (ControlOf(_control).Magic != ControlMagic || ControlOf(_control).Version != ControlVersion || !Refresh())
    {
        Close();
        return false;
    }
    return true;
}

void AssetIndexReader::Close()
{
    const ScopeLock lock(_lock);
    _view.Open({}, {});
    if (_delta.Data != nullptr)
    {
        PlatformSharedMemory::Close(_delta);
    }
    if (_base.Data != nullptr)
    {
        PlatformFile::Unmap(_base);
        _base = {};
    }
    if (_baseFile.IsValid())
    {
        PlatformFile::Close(_baseFile);
        _baseFile = {};
    }
    _basePath.Clear();
    if (_control.Data != nullptr)
    {
        PlatformSharedMemory::Close(_control);
    }
    _generation = 0;
}

uint64 AssetIndexReader::GetGeneration() const
{
    return _generation;
}

bool AssetIndexReader::Refresh()
{
    Published published;
    for (int32 attempt = 0; attempt < 16; ++attempt)
    {
        if (!ReadPublished(ControlOf(_control), published))
        {
            return false;
        }
        if (published.Generation == _generation)
        {
            return true;
        }
        SharedMemory delta;
        if (!PlatformSharedMemory::Open(SegmentName(_name.AsView(), published.Generation).CStr(),
                                        published.DeltaSize != 0 ? published.DeltaSize : 1, delta))
        {
            continue;
        }
        FileHandle baseFile = _baseFile;
        MappedRegion base = _base;
        const bool newBase = StringView(published.BasePath) != _basePath.AsView();
        if (newBase)
        {
            baseFile = PlatformFile::Open(published.BasePath, FileAccess::Read, FileMode::OpenExisting);
            if (!baseFile.IsValid() ||
                !PlatformFile::Map(baseFile, FileAccess::Read, 0, static_cast<usize>(published.BaseSize), base))
            {
                if (baseFile.IsValid())
                {
                    PlatformFile::Close(baseFile);
                }
                PlatformSharedMemory::Close(delta);
                continue;
            }
        }
        const ScopeLock lock(_lock);
        _view.Open(Span<const uint8>(static_cast<const uint8 *>(base.Data), static_cast<usize>(published.BaseSize)),
                   Span<const uint8>(static_cast<const uint8 *>(delta.Data), static_cast<usize>(published.DeltaSize)));
        if (_delta.Data != nullptr)
        {
            PlatformSharedMemory::Close(_delta);
        }
        _delta = delta;
        if (newBase)
        {
            if (_base.Data != nullptr)
            {
                PlatformFile::Unmap(_base);
            }
            if (_baseFile.IsValid())
            {
                PlatformFile::Close(_baseFile);
            }
            _base = base;
            _baseFile = baseFile;
            _basePath = published.BasePath;
        }
        _generation = published.Generation;
        return true;
    }
    return false;
}

bool AssetIndexReader::Read(FunctionRef<void(const AssetIndexView &view)> visitor)
{
    if (_control.Data == nullptr)
    {
        return false;
    }
    const Control &control = ControlOf(_control);
    if (control.Sequence.Load(MemoryOrder::Acquire) == 0 || (control.Generation != _generation && !Refresh()))
    {
        return false;
    }
    const SharedScopeLock lock(_lock);
    visitor(_view);
    return true;
}
} // namespace Marlin
