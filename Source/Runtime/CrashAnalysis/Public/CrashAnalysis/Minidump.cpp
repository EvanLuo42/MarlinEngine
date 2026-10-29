#include "CrashAnalysis/Minidump.h"

#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/Utf.h"
#include "Templates/Algorithm.h"

namespace Marlin
{
namespace
{
constexpr uint32 HeaderSignature = 0x504D444Du; // "MDMP"
constexpr uint32 ThreadListStream = 3;
constexpr uint32 ModuleListStream = 4;
constexpr uint32 MemoryListStream = 5;
constexpr uint32 ExceptionStream = 6;
constexpr uint32 SystemInfoStream = 7;
constexpr uint32 Memory64ListStream = 9;
constexpr uint32 PdbSignature = 0x53445352u; // "RSDS", a CodeView PDB 7.0 record
constexpr uint32 ElfSignature = 0x4270454Cu; // "LEpB", a build id record

constexpr usize HeaderSize = 32;
constexpr usize DirectoryEntrySize = 12;
constexpr usize ModuleSize = 108;
constexpr usize ThreadSize = 48;
constexpr usize MemoryDescriptorSize = 16;
constexpr usize ExceptionStreamSize = 168;

/// The value at *offset* of *bytes*, or zero past its end; minidumps are little-endian and unaligned.
template <typename T> T Load(Span<const uint8> bytes, usize offset)
{
    T value{};
    if (offset <= bytes.Count() && bytes.Count() - offset >= sizeof(T))
    {
        MemCopy(&value, bytes.Data() + offset, sizeof(T));
    }
    return value;
}

StringView FileNameOf(StringView path)
{
    usize start = path.Length();
    while (start > 0)
    {
        if (path[start - 1] == '/' || path[start - 1] == '\\')
        {
            break;
        }
        --start;
    }
    return path.Substring(start);
}
} // namespace

Minidump::Minidump() = default;

Minidump::~Minidump()
{
    Close();
}

bool Minidump::Open(StringView path, String &error)
{
    Close();
    const String file(path);
    _file = PlatformFile::Open(file.CStr(), FileAccess::Read, FileMode::OpenExisting, FileFlags::RandomAccess);
    uint64 size = 0;
    if (!_file.IsValid() || !PlatformFile::GetSize(_file, size) || size < HeaderSize ||
        !PlatformFile::Map(_file, FileAccess::Read, 0, 0, _region))
    {
        error = Format("Cannot read {0}", path);
        Close();
        return false;
    }
    _data = static_cast<const uint8 *>(_region.Data);
    _size = _region.Size;
    const Span<const uint8> header = Bytes(0, HeaderSize);
    if (Load<uint32>(header, 0) != HeaderSignature)
    {
        error = Format("{0} is not a minidump", path);
        Close();
        return false;
    }
    const auto streamCount = Load<uint32>(header, 8);
    const auto directoryOffset = Load<uint32>(header, 12);
    const Span<const uint8> directory = Bytes(directoryOffset, uint64{streamCount} * DirectoryEntrySize);
    if (directory.Count() != uint64{streamCount} * DirectoryEntrySize)
    {
        error = Format("{0} is not a minidump", path);
        Close();
        return false;
    }
    for (uint32 i = 0; i < streamCount; ++i)
    {
        const usize entry = i * DirectoryEntrySize;
        const auto type = Load<uint32>(directory, entry);
        const Span<const uint8> stream = Bytes(Load<uint32>(directory, entry + 8), Load<uint32>(directory, entry + 4));
        switch (type)
        {
        case ThreadListStream:
            ReadThreads(stream);
            break;
        case ModuleListStream:
            ReadModules(stream);
            break;
        case MemoryListStream:
            ReadMemoryList(stream);
            break;
        case Memory64ListStream:
            ReadMemory64List(stream);
            break;
        case ExceptionStream:
            ReadException(stream);
            break;
        case SystemInfoStream:
            ReadSystemInfo(stream);
            break;
        default:
            break;
        }
    }
    Sort(_ranges.Data(), _ranges.Count(), [](const Range &a, const Range &b) { return a.Start < b.Start; });
    return true;
}

void Minidump::Close()
{
    if (_region.Data != nullptr)
    {
        PlatformFile::Unmap(_region);
        _region = {};
    }
    if (_file.IsValid())
    {
        PlatformFile::Close(_file);
        _file = {};
    }
    _data = nullptr;
    _size = 0;
    _arch = MinidumpArch::Unknown;
    _modules.Clear();
    _threads.Clear();
    _ranges.Clear();
    _hasException = false;
    _exceptionThread = 0;
    _exceptionCode = 0;
    _exceptionAddress = 0;
    _exceptionContext = {};
}

const MinidumpModule *Minidump::FindModule(uint64 address) const
{
    for (const MinidumpModule &module : _modules)
    {
        if (address >= module.Base && address - module.Base < module.Size)
        {
            return &module;
        }
    }
    return nullptr;
}

usize Minidump::ReadMemory(uint64 address, void *buffer, usize size) const
{
    auto *target = static_cast<uint8 *>(buffer);
    usize copied = 0;
    while (copied < size)
    {
        const uint64 at = address + copied;
        usize index = UpperBound(_ranges.Data(), _ranges.Count(), at,
                                 [](uint64 value, const Range &range) { return value < range.Start; });
        if (index == 0)
        {
            break;
        }
        const Range &range = _ranges[index - 1];
        if (at - range.Start >= range.Size)
        {
            break;
        }
        const uint64 available = range.Size - (at - range.Start);
        const usize count = available < size - copied ? static_cast<usize>(available) : size - copied;
        const Span<const uint8> bytes = Bytes(range.Offset + (at - range.Start), count);
        if (bytes.Count() != count)
        {
            break;
        }
        MemCopy(target + copied, bytes.Data(), count);
        copied += count;
    }
    return copied;
}

Span<const uint8> Minidump::Bytes(uint64 offset, uint64 size) const
{
    if (offset > _size || size > _size - offset)
    {
        return {};
    }
    return {_data + offset, static_cast<usize>(size)};
}

String Minidump::ReadString(uint32 offset) const
{
    const auto length = Load<uint32>(Bytes(offset, 4), 0);
    const Span<const uint8> text = Bytes(uint64{offset} + 4, length);
    if (text.Count() != length || length % 2 != 0)
    {
        return {};
    }
    List<char16_t> units;
    units.Resize(length / 2);
    MemCopy(units.Data(), text.Data(), length);
    String path = Utf8::FromUtf16(units.Data(), units.Count());
    path.Replace('\\', '/');
    return path;
}

void Minidump::ReadModules(Span<const uint8> stream)
{
    const auto count = Load<uint32>(stream, 0);
    for (uint32 i = 0; i < count && 4 + (i + 1) * ModuleSize <= stream.Count(); ++i)
    {
        const Span<const uint8> entry = stream.Slice(4 + i * ModuleSize, ModuleSize);
        MinidumpModule &module = _modules.AddDefaulted();
        module.Base = Load<uint64>(entry, 0);
        module.Size = Load<uint32>(entry, 8);
        module.TimeStamp = Load<uint32>(entry, 16);
        module.Path = ReadString(Load<uint32>(entry, 20));
        const Span<const uint8> record = Bytes(Load<uint32>(entry, 80), Load<uint32>(entry, 76));
        const auto signature = Load<uint32>(record, 0);
        if (signature == PdbSignature && record.Count() > 24)
        {
            // The GUID's first three fields are little-endian in memory; a symbol store writes them big-endian.
            const uint8 order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
            for (const uint8 index : order)
            {
                module.DebugId.Add(record[4 + index]);
            }
            module.DebugAge = Load<uint32>(record, 20);
            const Span<const uint8> name = record.Slice(24);
            usize length = 0;
            while (length < name.Count() && name[length] != 0)
            {
                ++length;
            }
            module.DebugFile = String(FileNameOf(StringView(reinterpret_cast<const char *>(name.Data()), length)));
        }
        else if (signature == ElfSignature && record.Count() > 4)
        {
            module.DebugId.AddRange(record.Slice(4));
            module.DebugFile = String(FileNameOf(module.Path.AsView()));
        }
    }
}

void Minidump::ReadThreads(Span<const uint8> stream)
{
    const auto count = Load<uint32>(stream, 0);
    for (uint32 i = 0; i < count && 4 + (i + 1) * ThreadSize <= stream.Count(); ++i)
    {
        const Span<const uint8> entry = stream.Slice(4 + i * ThreadSize, ThreadSize);
        MinidumpThread &thread = _threads.AddDefaulted();
        thread.Id = Load<uint32>(entry, 0);
        thread.Context = Bytes(Load<uint32>(entry, 44), Load<uint32>(entry, 40));
        // The stack is usually in the memory list, and the offset here zero; some dumps keep it only here.
        const auto stackOffset = Load<uint32>(entry, 36);
        if (stackOffset != 0)
        {
            _ranges.Add({.Start = Load<uint64>(entry, 24), .Size = Load<uint32>(entry, 32), .Offset = stackOffset});
        }
    }
}

void Minidump::ReadMemoryList(Span<const uint8> stream)
{
    const auto count = Load<uint32>(stream, 0);
    for (uint32 i = 0; i < count && 4 + (i + 1) * MemoryDescriptorSize <= stream.Count(); ++i)
    {
        const usize entry = 4 + i * MemoryDescriptorSize;
        _ranges.Add({.Start = Load<uint64>(stream, entry),
                     .Size = Load<uint32>(stream, entry + 8),
                     .Offset = Load<uint32>(stream, entry + 12)});
    }
}

void Minidump::ReadMemory64List(Span<const uint8> stream)
{
    const auto count = Load<uint64>(stream, 0);
    uint64 offset = Load<uint64>(stream, 8);
    for (uint64 i = 0; i < count && 16 + (i + 1) * MemoryDescriptorSize <= stream.Count(); ++i)
    {
        const auto entry = static_cast<usize>(16 + i * MemoryDescriptorSize);
        const auto size = Load<uint64>(stream, entry + 8);
        _ranges.Add({.Start = Load<uint64>(stream, entry), .Size = size, .Offset = offset});
        offset += size;
    }
}

void Minidump::ReadException(Span<const uint8> stream)
{
    if (stream.Count() < ExceptionStreamSize)
    {
        return;
    }
    _hasException = true;
    _exceptionThread = Load<uint32>(stream, 0);
    _exceptionCode = Load<uint32>(stream, 8);
    _exceptionAddress = Load<uint64>(stream, 24);
    _exceptionContext = Bytes(Load<uint32>(stream, 164), Load<uint32>(stream, 160));
}

void Minidump::ReadSystemInfo(Span<const uint8> stream)
{
    switch (Load<uint16>(stream, 0))
    {
    case 9:
        _arch = MinidumpArch::X64;
        break;
    case 12:
        _arch = MinidumpArch::Arm64;
        break;
    default:
        _arch = MinidumpArch::Unknown;
        break;
    }
}
} // namespace Marlin
