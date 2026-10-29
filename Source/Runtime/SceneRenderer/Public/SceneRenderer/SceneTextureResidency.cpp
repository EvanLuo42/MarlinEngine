#include "SceneRenderer/SceneTextureResidency.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/AssertFormat.h"
#include "Math/MathUtility.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiDevice.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "Renderer/RenderFrame.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace
{
/// Bytes of texels Extract hands the render job in one frame; one texture always goes, however large.
constexpr uint64 MaxUploadBytesPerFrame = uint64{64} << 20;

RhiFormat GetRhiFormat(TextureFormat format, TextureColorSpace colorSpace)
{
    const bool srgb = colorSpace == TextureColorSpace::Srgb;
    switch (format)
    {
    case TextureFormat::RGBA8:
        return srgb ? RhiFormat::RGBA8UnormSrgb : RhiFormat::RGBA8Unorm;
    case TextureFormat::R8:
        return RhiFormat::R8Unorm;
    case TextureFormat::RG8:
        return RhiFormat::RG8Unorm;
    case TextureFormat::RGBA16Float:
        return RhiFormat::RGBA16Float;
    case TextureFormat::BC1:
        return srgb ? RhiFormat::BC1UnormSrgb : RhiFormat::BC1Unorm;
    case TextureFormat::BC3:
        return srgb ? RhiFormat::BC3UnormSrgb : RhiFormat::BC3Unorm;
    case TextureFormat::BC4:
        return RhiFormat::BC4Unorm;
    case TextureFormat::BC5:
        return RhiFormat::BC5Unorm;
    case TextureFormat::BC7:
        return srgb ? RhiFormat::BC7UnormSrgb : RhiFormat::BC7Unorm;
    case TextureFormat::BC6H:
        return RhiFormat::BC6HUfloat;
    case TextureFormat::ASTC4x4:
        return srgb ? RhiFormat::Astc4x4UnormSrgb : RhiFormat::Astc4x4Unorm;
    case TextureFormat::ASTC6x6:
        return srgb ? RhiFormat::Astc6x6UnormSrgb : RhiFormat::Astc6x6Unorm;
    case TextureFormat::ASTC8x8:
        return srgb ? RhiFormat::Astc8x8UnormSrgb : RhiFormat::Astc8x8Unorm;
    }
    return RhiFormat::Unknown;
}

/// Rows of texels, or of blocks, in a mip *height* texels high.
uint32 GetMipRows(TextureFormat format, uint32 height)
{
    const uint32 block = TextureFormats::GetBlockSize(format);
    return (height + block - 1) / block;
}

uint64 GetUploadSize(Span<const TextureMip> mips)
{
    uint64 size = 0;
    for (const TextureMip &mip : mips)
    {
        size += mip.Size;
    }
    return size;
}
} // namespace

struct SceneTextureResidency::Entry
{
    enum class State : uint8
    {
        /// Waiting for the asset to load.
        Waiting,
        /// Loaded: its tail is read and uploaded, then mips stream in and out.
        Active,
        Failed,
        /// No references: retiring, then free.
        Released,
    };

    AssetRef<Texture> Ref;
    Guid Uuid;
    uint32 References = 0;
    State Status = State::Waiting;
    /// The asset's description, kept while the entry lives.
    TextureFormat Format = TextureFormat::RGBA8;
    TextureColorSpace ColorSpace = TextureColorSpace::Srgb;
    String Name;
    List<TextureMip> Mips;
    /// Mips Tail on always stay; MaxDimensionLog2 is that of mip 0's larger side.
    uint32 Tail = 0;
    uint32 MaxDimensionLog2 = 0;
    /// The sampled texture holds mips First on; MipCount while it holds none.
    GpuTexture Gpu;
    uint32 First = 0;
    /// What replaces Gpu once its upload has run, holding mips NextFirst on; Busy from the read or shrink that makes
    /// it until then.
    GpuTexture Next;
    uint32 NextFirst = 0;
    bool Busy = false;
    bool Reading = false;
    BulkRequestId Request = 0;
    /// The finest mip the feedback asked for lately, and when it asked for it or finer.
    uint32 Wanted = 0;
    uint64 WantedFrame = 0;
    /// The frame some sample last needed it.
    uint64 LastNeeded = 0;
    uint32 FramesLeft = 0;
};

SceneTextureResidency::SceneTextureResidency(RhiDevice &device, BulkStreamer *streamer)
    : _device(device), _streamer(streamer)
{
    _uploadAllocator = MakeUnique<RhiUploadAllocator>(device);
    CreateSampler(_settings);
    CreateDefaults();
    RhiBufferDesc feedback;
    feedback.Size = uint64{MaxFeedbackTextures} * sizeof(uint32);
    feedback.Usage = RhiBufferUsage::ShaderResource | RhiBufferUsage::UnorderedAccess | RhiBufferUsage::CopySource |
                     RhiBufferUsage::CopyDest;
    feedback.Flags = RhiResourceFlags::Concurrent;
    feedback.DebugName = "Scene Texture Feedback";
    if (Result<RhiBuffer, RhiError> created = device.CreateBuffer(feedback); created.IsOk())
    {
        _feedback = created.Value();
        _feedbackAddress = device.GetBufferAddress(_feedback);
    }
    else
    {
        MARLIN_LOG(LogRenderer, Error, "The texture feedback buffer could not be created: {0}",
                   GetErrorName(created.Error()));
    }
    for (Readback &readback : _readbacks)
    {
        RhiBufferDesc desc;
        desc.Size = feedback.Size;
        desc.Usage = RhiBufferUsage::CopyDest;
        desc.Location = RhiMemoryLocation::Readback;
        desc.DebugName = "Scene Texture Feedback Readback";
        if (Result<RhiBuffer, RhiError> created = device.CreateBuffer(desc); created.IsOk())
        {
            readback.Buffer = created.Value();
        }
    }
}

SceneTextureResidency::~SceneTextureResidency()
{
    const ScopeLock lock(_lock);
    for (UniquePtr<Entry> &entry : _entries)
    {
        if (entry->Reading && _streamer != nullptr)
        {
            _streamer->Cancel(entry->Request);
        }
        DestroyGpuTexture(entry->Gpu);
        DestroyGpuTexture(entry->Next);
    }
    _entries.Clear();
    _entryByGuid.Clear();
    _uploads.Clear();
    _renderUploads.Clear();
    for (Retiring &retiring : _retiring)
    {
        DestroyGpuTexture(retiring.Texture);
        if (retiring.Sampler.IsValid())
        {
            _device.Destroy(retiring.Sampler);
        }
    }
    _retiring.Clear();
    for (GpuTexture &texture : _defaults)
    {
        DestroyGpuTexture(texture);
    }
    if (_sampler.IsValid())
    {
        _device.Destroy(_sampler);
    }
    if (_feedback.IsValid())
    {
        _device.Destroy(_feedback);
    }
    for (Readback &readback : _readbacks)
    {
        if (readback.Buffer.IsValid())
        {
            _device.Destroy(readback.Buffer);
        }
    }
    _uploadAllocator.Reset();
}

void SceneTextureResidency::CreateSampler(const SceneTextureSettings &settings)
{
    RhiSamplerDesc sampler;
    sampler.MaxAnisotropy = static_cast<uint8>(Clamp<uint32>(settings.MaxAnisotropy, 1, 16));
    sampler.MipLodBias = settings.MipBias;
    if (Result<RhiSampler, RhiError> created = _device.CreateSampler(sampler); created.IsOk())
    {
        // A new sampler is a new descriptor for every material to write.
        if (_sampler.IsValid())
        {
            Retiring &retiring = _retiring.AddDefaulted();
            retiring.Sampler = _sampler;
            retiring.FramesLeft = RetireFrames;
            ++_residentStamp;
        }
        _sampler = created.Value();
    }
    else
    {
        MARLIN_LOG(LogRenderer, Error, "The material sampler could not be created: {0}", GetErrorName(created.Error()));
    }
}

void SceneTextureResidency::CreateDefaults()
{
    static constexpr StringView Names[] = {"Scene Default White", "Scene Default Black", "Scene Default Flat Normal"};
    static constexpr uint8 Texels[][4] = {{255, 255, 255, 255}, {0, 0, 0, 255}, {128, 128, 255, 255}};
    for (uint32 index = 0; index < static_cast<uint32>(SceneDefaultTexture::Count); ++index)
    {
        if (!CreateGpuTexture(TextureFormat::RGBA8, TextureColorSpace::Linear, 1, 1, 1, Names[index], _defaults[index]))
        {
            continue;
        }
        Upload &upload = _uploads.AddDefaulted();
        upload.Target = _defaults[index].Texture;
        upload.Mips.Add({.Width = 1, .Height = 1, .RowPitch = 4, .Offset = 0, .Size = 4});
        upload.Bytes.AddRange(Span<const uint8>(Texels[index], 4));
    }
}

bool SceneTextureResidency::CreateGpuTexture(TextureFormat format, TextureColorSpace colorSpace, uint32 width,
                                             uint32 height, uint32 mipCount, StringView name, GpuTexture &created)
{
    RhiTextureDesc desc;
    desc.Format = GetRhiFormat(format, colorSpace);
    desc.Width = width;
    desc.Height = height;
    desc.MipCount = mipCount;
    desc.Usage = RhiTextureUsage::ShaderResource | RhiTextureUsage::CopyDest | RhiTextureUsage::CopySource;
    // Materials sample it on whichever queue shades, with no graph to transfer it between them.
    desc.Flags = RhiResourceFlags::Concurrent;
    desc.DebugName = name;
    Result<RhiTexture, RhiError> texture = _device.CreateTexture(desc);
    if (texture.IsError())
    {
        MARLIN_LOG(LogRenderer, Error, "Texture {0} ({1}x{2}, {3} mips) could not be created: {4}", name, width, height,
                   mipCount, GetErrorName(texture.Error()));
        return false;
    }
    RhiTextureViewDesc view;
    view.Texture = texture.Value();
    view.DebugName = name;
    Result<RhiResourceView, RhiError> resourceView = _device.CreateView(view);
    if (resourceView.IsError())
    {
        MARLIN_LOG(LogRenderer, Error, "Texture {0} has no view: {1}", name, GetErrorName(resourceView.Error()));
        _device.Destroy(texture.Value());
        return false;
    }
    created.Texture = texture.Value();
    created.View = resourceView.Value();
    return true;
}

void SceneTextureResidency::DestroyGpuTexture(GpuTexture &texture)
{
    if (texture.View.IsValid())
    {
        _device.Destroy(texture.View);
    }
    if (texture.Texture.IsValid())
    {
        _device.Destroy(texture.Texture);
    }
    texture = {};
}

void SceneTextureResidency::Retire(GpuTexture texture)
{
    if (!texture.Texture.IsValid())
    {
        return;
    }
    Retiring &retiring = _retiring.AddDefaulted();
    retiring.Texture = texture;
    retiring.FramesLeft = RetireFrames;
}

uint32 SceneTextureResidency::GetIndex(const Entry &entry) const
{
    return entry.Status == Entry::State::Active && entry.Gpu.View.IsValid()
               ? entry.Gpu.View.Index
               : GetDefaultTexture(SceneDefaultTexture::White);
}

uint64 SceneTextureResidency::GetBytes(const Entry &entry, uint32 first) const
{
    uint64 bytes = 0;
    for (uint32 mip = first; mip < entry.Mips.Count(); ++mip)
    {
        bytes += entry.Mips[mip].Size;
    }
    return bytes;
}

uint32 SceneTextureResidency::AcquireTexture(const AssetRef<Texture> &texture)
{
    const AssetSlot *slot = texture.GetSlot();
    if (slot == nullptr)
    {
        return GetDefaultTexture(SceneDefaultTexture::White);
    }
    const ScopeLock lock(_lock);
    if (const uint32 *found = _entryByGuid.Find(slot->GetGuid()))
    {
        Entry &entry = *_entries[*found];
        if (entry.Status == Entry::State::Released)
        {
            // Asked for again while retiring: it keeps what it has, or loads anew.
            if (entry.Mips.IsEmpty())
            {
                entry.Status = Entry::State::Waiting;
                _pendingReads.Add(*found);
            }
            else
            {
                entry.Status = Entry::State::Active;
            }
        }
        ++entry.References;
        return GetIndex(entry);
    }
    uint32 index = 0;
    if (!_freeEntries.IsEmpty())
    {
        index = _freeEntries.Pop();
    }
    else
    {
        index = static_cast<uint32>(_entries.Count());
        _entries.Add(MakeUnique<Entry>());
    }
    Entry &entry = *_entries[index];
    entry = Entry();
    entry.Ref = texture;
    entry.Uuid = slot->GetGuid();
    entry.References = 1;
    _entryByGuid.FindOrAdd(entry.Uuid) = index;
    _pendingReads.Add(index);
    return GetIndex(entry);
}

void SceneTextureResidency::ReleaseTexture(const AssetRef<Texture> &texture)
{
    const AssetSlot *slot = texture.GetSlot();
    if (slot == nullptr)
    {
        return;
    }
    const ScopeLock lock(_lock);
    const uint32 *found = _entryByGuid.Find(slot->GetGuid());
    const bool held = found != nullptr && _entries[*found]->References > 0;
    MARLIN_ASSERTF(held, "Texture {0} released more often than acquired", slot->GetGuid());
    if (held)
    {
        --_entries[*found]->References;
    }
}

uint32 SceneTextureResidency::GetTextureIndex(const AssetRef<Texture> &texture) const
{
    const AssetSlot *slot = texture.GetSlot();
    if (slot == nullptr)
    {
        return GetDefaultTexture(SceneDefaultTexture::White);
    }
    const ScopeLock lock(_lock);
    const uint32 *found = _entryByGuid.Find(slot->GetGuid());
    return found != nullptr ? GetIndex(*_entries[*found]) : GetDefaultTexture(SceneDefaultTexture::White);
}

uint64 SceneTextureResidency::GetFeedbackAddress(const AssetRef<Texture> &texture) const
{
    const AssetSlot *slot = texture.GetSlot();
    if (slot == nullptr || _feedbackAddress == 0)
    {
        return 0;
    }
    const ScopeLock lock(_lock);
    const uint32 *found = _entryByGuid.Find(slot->GetGuid());
    return found != nullptr && *found < MaxFeedbackTextures ? _feedbackAddress + uint64{*found} * sizeof(uint32) : 0;
}

uint32 SceneTextureResidency::GetResidentStamp() const
{
    const ScopeLock lock(_lock);
    return _residentStamp;
}

uint32 SceneTextureResidency::GetFirstResidentMip(const AssetRef<Texture> &texture) const
{
    const AssetSlot *slot = texture.GetSlot();
    const ScopeLock lock(_lock);
    const uint32 *found = slot != nullptr ? _entryByGuid.Find(slot->GetGuid()) : nullptr;
    return found != nullptr ? _entries[*found]->First : 0;
}

uint64 SceneTextureResidency::GetResidentBytes() const
{
    const ScopeLock lock(_lock);
    uint64 bytes = 0;
    for (const UniquePtr<Entry> &entry : _entries)
    {
        bytes += entry->Gpu.Texture.IsValid() ? GetBytes(*entry, entry->First) : 0;
    }
    return bytes;
}

void SceneTextureResidency::NoteNeeded(const AssetRef<Texture> &texture, uint32 resolutionLog2)
{
    const AssetSlot *slot = texture.GetSlot();
    const ScopeLock lock(_lock);
    if (const uint32 *found = slot != nullptr ? _entryByGuid.Find(slot->GetGuid()) : nullptr)
    {
        NoteNeeded(*_entries[*found], resolutionLog2);
    }
}

void SceneTextureResidency::NoteNeeded(Entry &entry, uint32 resolutionLog2)
{
    if (entry.Status != Entry::State::Active)
    {
        return;
    }
    // The mip whose larger side has the resolution the sample needed, biased as the sampler is.
    const float32 mip =
        static_cast<float32>(entry.MaxDimensionLog2) - static_cast<float32>(resolutionLog2) + _settings.MipBias;
    const uint32 wanted = static_cast<uint32>(Clamp(Math::Floor(mip), 0.0f, static_cast<float32>(entry.Tail)));
    entry.LastNeeded = _frame;
    // Finer at once; coarser only once nothing needed finer for RelaxFrames.
    if (wanted <= entry.Wanted || _frame - entry.WantedFrame > RelaxFrames)
    {
        entry.Wanted = wanted;
        entry.WantedFrame = _frame;
    }
}

void SceneTextureResidency::Update(const SceneTextureSettings &settings)
{
    const ScopeLock lock(_lock);
    ++_frame;
    if (!_hasSettings || settings.MaxAnisotropy != _settings.MaxAnisotropy || settings.MipBias != _settings.MipBias)
    {
        if (_hasSettings)
        {
            CreateSampler(settings);
        }
    }
    _hasSettings = true;
    _settings = settings;
    for (usize index = 0; index < _retiring.Count();)
    {
        Retiring &retiring = _retiring[index];
        if (--retiring.FramesLeft > 0)
        {
            ++index;
            continue;
        }
        DestroyGpuTexture(retiring.Texture);
        if (retiring.Sampler.IsValid())
        {
            _device.Destroy(retiring.Sampler);
        }
        _retiring.RemoveAtSwap(index);
    }
    for (uint32 index = 0; index < _entries.Count(); ++index)
    {
        Entry &entry = *_entries[index];
        if (!entry.Ref.IsValid())
        {
            continue;
        }
        if (entry.References == 0 && entry.Status != Entry::State::Released)
        {
            entry.Status = Entry::State::Released;
            entry.FramesLeft = RetireFrames;
            continue;
        }
        if (entry.Status != Entry::State::Released || --entry.FramesLeft > 0)
        {
            continue;
        }
        // No frame in flight samples it any more, and the upload of one never handed over goes with it.
        if (entry.Reading && _streamer != nullptr)
        {
            _streamer->Cancel(entry.Request);
            --_readsInFlight;
        }
        for (usize upload = 0; upload < _uploads.Count();)
        {
            if (_uploads[upload].Entry == index)
            {
                _uploads.RemoveAt(upload);
                continue;
            }
            ++upload;
        }
        _uploading.Remove(index);
        DestroyGpuTexture(entry.Gpu);
        DestroyGpuTexture(entry.Next);
        _entryByGuid.Remove(entry.Uuid);
        entry = Entry();
        _freeEntries.Add(index);
    }
    for (usize position = 0; position < _pendingReads.Count();)
    {
        const uint32 index = _pendingReads[position];
        Entry &entry = *_entries[index];
        if (entry.Status != Entry::State::Waiting)
        {
            _pendingReads.RemoveAtSwap(position);
            continue;
        }
        const AssetSlot *slot = entry.Ref.GetSlot();
        const AssetState state = slot != nullptr ? slot->GetState() : AssetState::Failed;
        if (state == AssetState::Loading || state == AssetState::Unloaded)
        {
            ++position;
            continue;
        }
        _pendingReads.RemoveAtSwap(position);
        const Texture *texture = entry.Ref.Get();
        bool valid =
            state != AssetState::Failed && texture != nullptr && !texture->Mips.IsEmpty() && _streamer != nullptr;
        for (uint32 mip = 0; valid && mip < texture->Mips.Count(); ++mip)
        {
            const TextureMip &source = texture->Mips[mip];
            const uint64 rows = GetMipRows(texture->Format, source.Height);
            // Largest first and back to back, so that any run of them is one read.
            valid = source.Width != 0 && source.Height != 0 && source.RowPitch != 0 &&
                    source.Size >= rows * source.RowPitch &&
                    (mip == 0 || source.Offset == texture->Mips[mip - 1].Offset + texture->Mips[mip - 1].Size);
        }
        if (!valid)
        {
            MARLIN_LOG(LogRenderer, Warning, "Texture {0} did not load and samples as white", entry.Uuid);
            entry.Status = Entry::State::Failed;
            continue;
        }
        entry.Format = texture->Format;
        entry.ColorSpace = texture->ColorSpace;
        entry.Name = String(texture->Name.AsView());
        entry.Mips.AddRange(Span<const TextureMip>(texture->Mips.Data(), texture->Mips.Count()));
        const uint32 count = static_cast<uint32>(entry.Mips.Count());
        entry.Tail = count > TailMips ? count - TailMips : 0;
        entry.MaxDimensionLog2 = static_cast<uint32>(FloorLog2(Max(entry.Mips[0].Width, entry.Mips[0].Height)));
        entry.First = count;
        entry.Wanted = entry.Tail;
        entry.WantedFrame = _frame;
        entry.LastNeeded = _frame;
        entry.Status = Entry::State::Active;
    }
    Stream();
}

void SceneTextureResidency::Stream()
{
    struct Candidate
    {
        uint32 Index = 0;
        uint32 Target = 0;
    };
    List<Candidate> grow;
    uint64 resident = 0;
    for (uint32 index = 0; index < _entries.Count(); ++index)
    {
        Entry &entry = *_entries[index];
        if (entry.Status != Entry::State::Active)
        {
            continue;
        }
        resident += GetBytes(entry, entry.Busy ? Min(entry.First, entry.NextFirst) : entry.First);
        if (entry.Busy)
        {
            continue;
        }
        if (_settings.Streaming && _frame - entry.LastNeeded > UnseenFrames)
        {
            entry.Wanted = entry.Tail;
        }
        const uint32 target = _settings.Streaming ? Min(entry.Wanted, entry.Tail) : 0;
        if (target < entry.First)
        {
            grow.Add({index, target});
        }
        else if (target > entry.First)
        {
            resident -= GetBytes(entry, entry.First) - GetBytes(entry, target);
            StartShrink(index, target);
        }
    }
    // The textures needed most lately first; a texture with nothing resident reads its tail whatever the pool holds.
    grow.Sort([this](const Candidate &a, const Candidate &b) {
        const Entry &x = *_entries[a.Index];
        const Entry &y = *_entries[b.Index];
        const bool xEmpty = x.First == x.Mips.Count();
        const bool yEmpty = y.First == y.Mips.Count();
        return xEmpty != yEmpty ? xEmpty : x.LastNeeded > y.LastNeeded;
    });
    for (const Candidate &candidate : grow)
    {
        Entry &entry = *_entries[candidate.Index];
        if (entry.Busy)
        {
            // It gave up a mip for another this frame.
            continue;
        }
        const bool empty = entry.First == entry.Mips.Count();
        if (_readsInFlight >= MaxReadsInFlight && !empty)
        {
            break;
        }
        uint32 target = candidate.Target;
        if (_settings.Streaming && !empty)
        {
            // Within the pool: those needed longer ago give up their finest mip, then this one takes what fits.
            const uint64 have = GetBytes(entry, entry.First);
            while (resident + GetBytes(entry, target) - have > _settings.PoolSize)
            {
                uint32 oldest = ~0u;
                for (uint32 index = 0; index < _entries.Count(); ++index)
                {
                    const Entry &other = *_entries[index];
                    if (other.Status == Entry::State::Active && !other.Busy && other.First < other.Tail &&
                        other.LastNeeded < entry.LastNeeded &&
                        (oldest == ~0u || other.LastNeeded < _entries[oldest]->LastNeeded))
                    {
                        oldest = index;
                    }
                }
                if (oldest == ~0u)
                {
                    break;
                }
                Entry &victim = *_entries[oldest];
                resident -= victim.Mips[victim.First].Size;
                StartShrink(oldest, victim.First + 1);
            }
            while (target < entry.First && resident + GetBytes(entry, target) - have > _settings.PoolSize)
            {
                ++target;
            }
            if (target == entry.First)
            {
                continue;
            }
            resident += GetBytes(entry, target) - have;
        }
        else
        {
            resident += GetBytes(entry, target) - GetBytes(entry, entry.First);
        }
        StartRead(candidate.Index, target);
    }
    // Still over the pool: the textures needed longest ago drop their finest mip, one each per frame.
    while (_settings.Streaming && resident > _settings.PoolSize)
    {
        uint32 oldest = ~0u;
        for (uint32 index = 0; index < _entries.Count(); ++index)
        {
            const Entry &other = *_entries[index];
            if (other.Status == Entry::State::Active && !other.Busy && other.First < other.Tail &&
                (oldest == ~0u || other.LastNeeded < _entries[oldest]->LastNeeded))
            {
                oldest = index;
            }
        }
        if (oldest == ~0u)
        {
            break;
        }
        Entry &victim = *_entries[oldest];
        resident -= victim.Mips[victim.First].Size;
        StartShrink(oldest, victim.First + 1);
    }
}

void SceneTextureResidency::StartRead(uint32 index, uint32 first)
{
    Entry &entry = *_entries[index];
    const TextureMip &from = entry.Mips[first];
    const TextureMip &to = entry.Mips[entry.First - 1];
    entry.Busy = true;
    entry.Reading = true;
    entry.NextFirst = first;
    ++_readsInFlight;
    entry.Request = _streamer->Request(
        *entry.Ref.GetSlot(), from.Offset, to.Offset + to.Size - from.Offset, 1.0f,
        [this, index](BulkRequestId /*request*/, SharedPtr<const BulkBytes> bytes) {
            OnTextureRead(index, Move(bytes));
        },
        IoPriority::Streaming);
}

void SceneTextureResidency::StartShrink(uint32 index, uint32 first)
{
    Entry &entry = *_entries[index];
    const TextureMip &top = entry.Mips[first];
    const uint32 count = static_cast<uint32>(entry.Mips.Count());
    if (!CreateGpuTexture(entry.Format, entry.ColorSpace, top.Width, top.Height, count - first, entry.Name.AsView(),
                          entry.Next))
    {
        return;
    }
    entry.Busy = true;
    entry.NextFirst = first;
    Upload &upload = _uploads.AddDefaulted();
    upload.Target = entry.Next.Texture;
    upload.Format = entry.Format;
    upload.CopyFrom = entry.Gpu.Texture;
    upload.CopyFromMip = first - entry.First;
    upload.CopyCount = count - first;
    upload.Entry = index;
}

void SceneTextureResidency::OnTextureRead(uint32 index, SharedPtr<const BulkBytes> bytes)
{
    const ScopeLock lock(_lock);
    Entry &entry = *_entries[index];
    if (!entry.Reading)
    {
        return;
    }
    entry.Reading = false;
    --_readsInFlight;
    const uint32 count = static_cast<uint32>(entry.Mips.Count());
    const uint32 first = entry.NextFirst;
    const uint64 start = entry.Mips[first].Offset;
    const TextureMip &last = entry.Mips[entry.First - 1];
    if (entry.Status != Entry::State::Active || bytes == nullptr ||
        bytes->GetBytes().Count() < last.Offset + last.Size - start)
    {
        if (entry.Status == Entry::State::Active)
        {
            MARLIN_LOG(LogRenderer, Warning, "The texels of texture {0} could not be read", entry.Uuid);
            entry.Status = entry.Gpu.Texture.IsValid() ? Entry::State::Active : Entry::State::Failed;
        }
        entry.Busy = false;
        return;
    }
    const TextureMip &top = entry.Mips[first];
    if (!CreateGpuTexture(entry.Format, entry.ColorSpace, top.Width, top.Height, count - first, entry.Name.AsView(),
                          entry.Next))
    {
        entry.Busy = false;
        return;
    }
    Upload &upload = _uploads.AddDefaulted();
    upload.Target = entry.Next.Texture;
    upload.Format = entry.Format;
    upload.Mips.AddRange(Span<const TextureMip>(entry.Mips.Data() + first, entry.First - first));
    upload.Source = Move(bytes);
    upload.SourceOffset = start;
    upload.CopyFrom = entry.Gpu.Texture;
    upload.CopyFromMip = 0;
    upload.CopyCount = count - entry.First;
    upload.Entry = index;
}

void SceneTextureResidency::ProcessReadbacks()
{
    if (_lastReadback != ~0u && _readbacks[_lastReadback].Written)
    {
        Readback &readback = _readbacks[_lastReadback];
        readback.SubmitValue = _device.GetSubmittedValues().Values[static_cast<uint32>(RhiQueueType::Graphics)];
        readback.InFlight = true;
        readback.Written = false;
    }
    const uint64 completed = _device.GetCompletedValues().Values[static_cast<uint32>(RhiQueueType::Graphics)];
    for (Readback &readback : _readbacks)
    {
        if (!readback.InFlight || readback.SubmitValue > completed)
        {
            continue;
        }
        readback.InFlight = false;
        const auto *words = reinterpret_cast<const uint32 *>(_device.GetMappedData(readback.Buffer));
        if (words == nullptr)
        {
            continue;
        }
        const uint32 count = Min(static_cast<uint32>(_entries.Count()), MaxFeedbackTextures);
        for (uint32 index = 0; index < count; ++index)
        {
            // The resolution a sample needed plus one; zero where none sampled it.
            if (words[index] != 0)
            {
                NoteNeeded(*_entries[index], words[index] - 1);
            }
        }
    }
    _renderReadback = ~0u;
    for (uint32 index = 0; index < 3; ++index)
    {
        if (!_readbacks[index].InFlight && _readbacks[index].Buffer.IsValid())
        {
            _renderReadback = index;
            break;
        }
    }
    _lastReadback = _renderReadback;
}

void SceneTextureResidency::Extract()
{
    const ScopeLock lock(_lock);
    ProcessReadbacks();
    // The last render job recorded these, so every later frame samples them uploaded.
    for (const uint32 index : _uploading)
    {
        Entry &entry = *_entries[index];
        if (!entry.Busy || !entry.Next.Texture.IsValid())
        {
            continue;
        }
        Retire(entry.Gpu);
        entry.Gpu = entry.Next;
        entry.Next = {};
        entry.First = entry.NextFirst;
        entry.Busy = false;
        ++_residentStamp;
    }
    _uploading.Clear();
    uint64 bytes = 0;
    for (usize position = 0; position < _uploads.Count();)
    {
        Upload &upload = _uploads[position];
        // A released texture's upload waits in case it is asked for again before it goes.
        if (upload.Entry != ~0u && _entries[upload.Entry]->Status == Entry::State::Released)
        {
            ++position;
            continue;
        }
        const uint64 size = GetUploadSize(upload.Mips.AsSpan());
        if (bytes > 0 && bytes + size > MaxUploadBytesPerFrame)
        {
            break;
        }
        bytes += size;
        if (upload.Entry != ~0u)
        {
            _uploading.Add(upload.Entry);
        }
        _renderUploads.Add(Move(upload));
        _uploads.RemoveAt(position);
    }
}

void SceneTextureResidency::AddPasses(RenderGraphBuilder &graph)
{
    // The last frame has been submitted: its upload memory comes back once the GPU is done with it.
    _uploadAllocator->EndFrame();
    if (!_renderUploads.IsEmpty())
    {
        graph.AddPass(
            "Scene Texture Upload", RhiQueueType::Graphics, [](RenderGraphPassBuilder &pass) { pass.NeverCull(); },
            [this](RenderGraphContext &context) { RecordUploads(context.List); });
    }
    if (_renderReadback >= 3 || !_feedback.IsValid())
    {
        return;
    }
    // What the last frame's samples needed, for the main thread once the GPU is done, and a clear feedback for this
    // frame's. Material Resolve writes it through addresses the graph does not see, hence the global barriers.
    Readback &readback = _readbacks[_renderReadback];
    readback.Written = true;
    const RhiBuffer feedback = _feedback;
    const RhiBuffer target = readback.Buffer;
    graph.AddPass(
        "Scene Texture Feedback", RhiQueueType::Graphics, [](RenderGraphPassBuilder &pass) { pass.NeverCull(); },
        [feedback, target](RenderGraphContext &context) {
            const uint64 size = uint64{MaxFeedbackTextures} * sizeof(uint32);
            RhiGlobalBarrier toCopy;
            toCopy.SyncBefore = RhiSync::AllShading;
            toCopy.AccessBefore = RhiAccess::UnorderedAccess;
            toCopy.SyncAfter = RhiSync::Copy;
            toCopy.AccessAfter = RhiAccess::CopySource | RhiAccess::CopyDest;
            RhiBarriers before;
            before.Global = Span<const RhiGlobalBarrier>(&toCopy, 1);
            context.List.Barrier(before);
            context.List.CopyBuffer(target, 0, feedback, 0, size);
            RhiGlobalBarrier copied;
            copied.SyncBefore = RhiSync::Copy;
            copied.AccessBefore = RhiAccess::CopySource;
            copied.SyncAfter = RhiSync::Copy;
            copied.AccessAfter = RhiAccess::CopyDest;
            RhiBarriers between;
            between.Global = Span<const RhiGlobalBarrier>(&copied, 1);
            context.List.Barrier(between);
            context.List.FillBuffer(feedback, 0, size, 0);
            RhiGlobalBarrier after[2];
            after[0].SyncBefore = RhiSync::Copy;
            after[0].AccessBefore = RhiAccess::CopyDest;
            after[0].SyncAfter = RhiSync::AllShading;
            after[0].AccessAfter = RhiAccess::UnorderedAccess;
            after[1].SyncBefore = RhiSync::Copy;
            after[1].AccessBefore = RhiAccess::CopyDest;
            after[1].SyncAfter = RhiSync::Host;
            after[1].AccessAfter = RhiAccess::HostRead;
            RhiBarriers done;
            done.Global = Span<const RhiGlobalBarrier>(after, 2);
            context.List.Barrier(done);
        });
}

void SceneTextureResidency::RecordUploads(RhiCommandList &list)
{
    if (_renderUploads.IsEmpty())
    {
        return;
    }
    const RhiDeviceLimits &limits = _device.GetLimits();
    const uint64 offsetAlignment = Max<uint64>(limits.CopyOffsetAlignment, 16);
    const uint32 pitchAlignment = Max<uint32>(limits.CopyRowPitchAlignment, 1);
    // The new textures to CopyDest, and the ones they copy mips from to CopySource and back.
    List<RhiTextureBarrier> barriers;
    for (const Upload &upload : _renderUploads)
    {
        RhiTextureBarrier &barrier = barriers.AddDefaulted();
        barrier.Texture = upload.Target;
        barrier.SyncAfter = RhiSync::Copy;
        barrier.AccessAfter = RhiAccess::CopyDest;
        barrier.LayoutBefore = RhiLayout::Undefined;
        barrier.LayoutAfter = RhiLayout::CopyDest;
        barrier.Discard = true;
        if (upload.CopyCount > 0 && upload.CopyFrom.IsValid())
        {
            RhiTextureBarrier &source = barriers.AddDefaulted();
            source.Texture = upload.CopyFrom;
            source.SyncBefore = RhiSync::AllShading;
            source.AccessBefore = RhiAccess::ShaderResource;
            source.LayoutBefore = RhiLayout::ShaderResource;
            source.SyncAfter = RhiSync::Copy;
            source.AccessAfter = RhiAccess::CopySource;
            source.LayoutAfter = RhiLayout::CopySource;
        }
    }
    RhiBarriers before;
    before.Textures = barriers.AsSpan();
    list.Barrier(before);
    for (const Upload &upload : _renderUploads)
    {
        const Span<const uint8> bytes = upload.Source != nullptr ? upload.Source->GetBytes() : upload.Bytes.AsSpan();
        for (uint32 mipIndex = 0; mipIndex < upload.Mips.Count(); ++mipIndex)
        {
            const TextureMip &mip = upload.Mips[mipIndex];
            const uint32 rows = GetMipRows(upload.Format, mip.Height);
            const uint32 pitch = (mip.RowPitch + pitchAlignment - 1) / pitchAlignment * pitchAlignment;
            const RhiUploadAllocation allocation = _uploadAllocator->Allocate(uint64{pitch} * rows, offsetAlignment);
            if (allocation.Data == nullptr)
            {
                MARLIN_LOG(LogRenderer, Error, "Out of upload memory for a {0}x{1} texture mip", mip.Width, mip.Height);
                continue;
            }
            const uint8 *source = bytes.Data() + (mip.Offset - upload.SourceOffset);
            if (pitch == mip.RowPitch)
            {
                MemCopy(allocation.Data, source, uint64{pitch} * rows);
            }
            else
            {
                for (uint32 row = 0; row < rows; ++row)
                {
                    MemCopy(allocation.Data + uint64{row} * pitch, source + uint64{row} * mip.RowPitch, mip.RowPitch);
                }
            }
            RhiTextureRegion region;
            region.Mip = mipIndex;
            RhiBufferTextureLayout layout;
            layout.Offset = allocation.Offset;
            layout.RowPitch = pitch;
            list.CopyBufferToTexture(upload.Target, region, allocation.Buffer, layout);
        }
        if (upload.CopyFrom.IsValid())
        {
            const uint32 uploaded = static_cast<uint32>(upload.Mips.Count());
            for (uint32 mip = 0; mip < upload.CopyCount; ++mip)
            {
                RhiTextureRegion to;
                to.Mip = uploaded + mip;
                RhiTextureRegion from;
                from.Mip = upload.CopyFromMip + mip;
                list.CopyTexture(upload.Target, to, upload.CopyFrom, from);
            }
        }
    }
    for (RhiTextureBarrier &barrier : barriers)
    {
        const bool source = barrier.LayoutAfter == RhiLayout::CopySource;
        barrier.SyncBefore = RhiSync::Copy;
        barrier.AccessBefore = source ? RhiAccess::CopySource : RhiAccess::CopyDest;
        barrier.LayoutBefore = source ? RhiLayout::CopySource : RhiLayout::CopyDest;
        barrier.SyncAfter = RhiSync::AllShading;
        barrier.AccessAfter = RhiAccess::ShaderResource;
        barrier.LayoutAfter = RhiLayout::ShaderResource;
        barrier.Discard = false;
    }
    RhiBarriers after;
    after.Textures = barriers.AsSpan();
    list.Barrier(after);
    _renderUploads.Clear();
}
} // namespace Marlin
