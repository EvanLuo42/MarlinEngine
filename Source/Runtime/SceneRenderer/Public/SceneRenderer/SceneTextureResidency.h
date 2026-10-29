#pragma once

#include "Asset/AssetRef.h"
#include "Asset/BulkStreamer.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "RHI/RhiFormat.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiUploadAllocator.h"
#include "Texture/Texture.h"

namespace Marlin
{
class RenderGraphBuilder;
class RhiCommandList;
class RhiDevice;

/// The textures a material samples when it names none of its own, made at startup: 1x1 RGBA8 each.
enum class SceneDefaultTexture : uint8
{
    /// (1, 1, 1, 1): the neutral color and mask.
    White,
    /// (0, 0, 0, 1).
    Black,
    /// (0.5, 0.5, 1, 1): a tangent-space normal facing straight out.
    FlatNormal,
    Count,
};

/// How SceneTextureResidency keeps textures, from SceneRendererSubsystem's settings each frame.
struct SceneTextureSettings
{
    /// Whether textures stream their mips (UE's r.TextureStreaming); off, every texture loads whole.
    bool Streaming = true;
    /// Bytes of mips streaming keeps resident (r.Streaming.PoolSize); the tails always stay, over it if need be.
    uint64 PoolSize = uint64{1} << 30;
    /// Added to the mip every sample picks and every texture streams (r.MipMapLODBias): positive is blurrier.
    float32 MipBias = 0;
    /// The material sampler's anisotropy (r.MaxAnisotropy): one turns it off.
    uint32 MaxAnisotropy = 16;
};

/// Texture assets resident on the GPU for materials to sample bindlessly (Documentation/Architecture/07 §4.4): each
/// is a texture of its own format and color space holding its resident mips, with a persistent view whose descriptor
/// index materials write into their parameter blocks. Owned by SceneRendererSubsystem.
///
/// Streaming: a texture first loads its tail, the mips of TailMips and smaller. Material Resolve notes per texture
/// the finest resolution its samples needed (MaterialTexture2D.Feedback, a word per texture in a GPU buffer), which
/// comes back a few frames later; finer mips are read from the asset's bulk data, and the texture is made anew at its
/// new size with the read mips uploaded and the resident ones copied over on the GPU. A texture asked for coarser
/// mips for RelaxFrames frames, or not seen for UnseenFrames, drops its finest; over the pool, those needed longest ago
/// drop theirs first. The descriptor index changes with the texture, so GetResidentStamp grows.
class SCENERENDERER_API SceneTextureResidency
{
public:
    /// Frames a texture nothing references any more keeps its descriptor, so no frame in flight samples a reused one.
    static constexpr uint32 RetireFrames = 3;
    /// Mips of the tail, which stays resident: 64x64 and smaller.
    static constexpr uint32 TailMips = 7;
    /// Frames a texture keeps mips finer than its samples need before it drops them.
    static constexpr uint32 RelaxFrames = 30;
    /// Frames after which a texture no sample needed drops back to its tail.
    static constexpr uint32 UnseenFrames = 300;
    /// Textures with a feedback word; the others stream as if never seen.
    static constexpr uint32 MaxFeedbackTextures = 1u << 14;
    /// Reads of finer mips at once.
    static constexpr uint32 MaxReadsInFlight = 16;

    /// Without a *streamer* no asset loads. The defaults are made at once and uploaded by the first frame, before
    /// its other passes on the Graphics queue.
    SceneTextureResidency(RhiDevice &device, BulkStreamer *streamer);
    ~SceneTextureResidency();
    MARLIN_NONCOPYABLE(SceneTextureResidency);

    // Thread-safe, for the extract systems.

    /// The descriptor index of *texture*, adding a reference: White's until it is uploaded, or if it fails.
    [[nodiscard]] uint32 AcquireTexture(const AssetRef<Texture> &texture);
    void ReleaseTexture(const AssetRef<Texture> &texture);
    /// The descriptor index of a texture acquired before, without a reference: White's while it is not resident.
    [[nodiscard]] uint32 GetTextureIndex(const AssetRef<Texture> &texture) const;
    /// The address of the feedback word of a texture acquired before (MaterialTexture2D.Feedback); zero for none.
    [[nodiscard]] uint64 GetFeedbackAddress(const AssetRef<Texture> &texture) const;
    /// Grows whenever a texture's or the sampler's descriptor changes, so what holds indices looks them up again.
    [[nodiscard]] uint32 GetResidentStamp() const;
    /// The finest resident mip of a texture acquired before; its mip count while none is.
    [[nodiscard]] uint32 GetFirstResidentMip(const AssetRef<Texture> &texture) const;
    /// Bytes of the resident mips of every texture.
    [[nodiscard]] uint64 GetResidentBytes() const;
    /// Notes that a sample of *texture* needed 2^*resolutionLog2* texels across its UV range, as the feedback does.
    void NoteNeeded(const AssetRef<Texture> &texture, uint32 resolutionLog2);

    [[nodiscard]] uint32 GetDefaultTexture(SceneDefaultTexture texture) const
    {
        return _defaults[static_cast<uint32>(texture)].View.Index;
    }

    /// The sampler materials sample with: trilinear, anisotropic and biased as the settings say, wrapping.
    [[nodiscard]] uint32 GetMaterialSampler() const
    {
        return _sampler.Index;
    }

    // The frame, from SceneRendererSubsystem.

    /// Main thread, every frame: starts reading textures whose assets have loaded, streams mips in and out as the
    /// feedback and *settings* say, and retires released textures.
    void Update(const SceneTextureSettings &settings = {});
    /// Main thread while no render job runs: takes in the feedback the GPU has finished, hands the uploads made since
    /// the last call to the render job, and makes resident the ones the last render job uploaded.
    void Extract();
    /// Render job: a pass that uploads what Extract handed over, and one that reads back and clears the feedback.
    void AddPasses(RenderGraphBuilder &graph);
    /// Records the uploads Extract handed over into *list*, a Graphics list, leaving each texture in ShaderResource;
    /// what AddPasses's pass does, for tests and tools without a graph.
    void RecordUploads(RhiCommandList &list);

private:
    struct Entry;
    /// A texture's new GPU texture: the mips to copy from bulk bytes (or from Bytes for the defaults) into its first
    /// mips, then CopyCount mips copied from CopyFrom's CopyFromMip on.
    struct Upload
    {
        RhiTexture Target;
        TextureFormat Format = TextureFormat::RGBA8;
        List<TextureMip> Mips;
        SharedPtr<const BulkBytes> Source;
        /// Where Mips' offsets count from in Source.
        uint64 SourceOffset = 0;
        List<uint8> Bytes;
        RhiTexture CopyFrom;
        uint32 CopyFromMip = 0;
        uint32 CopyCount = 0;
        uint32 Entry = ~0u;
    };
    struct GpuTexture
    {
        RhiTexture Texture;
        RhiResourceView View;
    };
    /// Something to destroy once no frame in flight uses it.
    struct Retiring
    {
        GpuTexture Texture;
        RhiSampler Sampler;
        uint32 FramesLeft = 0;
    };
    /// A GPU-to-CPU copy of a frame's feedback, read once the GPU has finished that frame.
    struct Readback
    {
        RhiBuffer Buffer;
        uint64 SubmitValue = 0;
        bool Written = false;
        bool InFlight = false;
    };

    /// Makes a texture and its view, false if the device cannot; *name* names them in tools.
    bool CreateGpuTexture(TextureFormat format, TextureColorSpace colorSpace, uint32 width, uint32 height,
                          uint32 mipCount, StringView name, GpuTexture &created);
    void OnTextureRead(uint32 entry, SharedPtr<const BulkBytes> bytes);
    void CreateDefaults();
    void CreateSampler(const SceneTextureSettings &settings);
    [[nodiscard]] uint32 GetIndex(const Entry &entry) const;
    void DestroyGpuTexture(GpuTexture &texture);
    void Retire(GpuTexture texture);
    /// Under the lock: the bytes of mips *first* on of *entry*.
    [[nodiscard]] uint64 GetBytes(const Entry &entry, uint32 first) const;
    /// Under the lock: starts reading mips *first* to the resident ones of *entry*, or making it anew with mips *first*
    /// on when they are resident already.
    void StartRead(uint32 index, uint32 first);
    void StartShrink(uint32 index, uint32 first);
    /// Under the lock: what streaming does this frame.
    void Stream();
    void NoteNeeded(Entry &entry, uint32 resolutionLog2);
    void ProcessReadbacks();

    RhiDevice &_device;
    BulkStreamer *_streamer;
    GpuTexture _defaults[static_cast<uint32>(SceneDefaultTexture::Count)];
    RhiSampler _sampler;
    UniquePtr<RhiUploadAllocator> _uploadAllocator;
    RhiBuffer _feedback;
    uint64 _feedbackAddress = 0;
    Readback _readbacks[3];
    uint32 _renderReadback = ~0u;
    uint32 _lastReadback = ~0u;

    mutable SpinLock _lock;
    SceneTextureSettings _settings;
    bool _hasSettings = false;
    uint64 _frame = 0;
    uint32 _readsInFlight = 0;
    List<UniquePtr<Entry>> _entries;
    HashMap<Guid, uint32> _entryByGuid;
    List<uint32> _freeEntries;
    /// Entries whose assets were asked for or still load, which Update starts reading once loaded.
    List<uint32> _pendingReads;
    /// Read and waiting for Extract.
    List<Upload> _uploads;
    /// Handed to the render job by the last Extract; resident at the next.
    List<uint32> _uploading;
    List<Retiring> _retiring;
    uint32 _residentStamp = 0;

    // Render job.
    List<Upload> _renderUploads;
};
} // namespace Marlin
