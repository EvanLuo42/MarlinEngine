#pragma once

#include "Asset/AssetRef.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "HAL/PlatformSync.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "RHI/RhiCommandList.h"
#include "RHI/RhiResources.h"
#include "RHI/RhiUploadAllocator.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderDeclaration.h"
#include "String/String.h"
#include "Shader/ShaderLibrary.gen.h"

namespace Marlin
{
class RhiDevice;
class RhiPipelineLibrary;
class RhiSubsystem;
struct RhiPipelineRecord;

/// Loads global shaders by name and makes their pipelines (MarlinLanguage.md §1.6). It picks the device's profile at
/// startup, the first of Engine.Shaders.Profiles whose RHI the device is and whose features it has. A shader loads
/// like an asset under GetGlobalShaderGuid of its name: from packages when packaged, through the indexer's
/// Shader.Locate in development, where source edits reload it and its pipelines are made again. Pipelines go through
/// the RhiPipelineLibrary, so they compile on jobs and land in the pipeline list. A compute shader has one pipeline;
/// graphics stages make one per ShaderGraphicsPipelineDesc they are drawn with. Until a shader has loaded and its
/// pipeline is ready, dispatches of it are skipped and return false, like draws of a pipeline still compiling
/// (07 §1.4). Thread-safe: passes dispatch from their recording jobs.
///
/// Shaders compiled with debug output write `Debug.Print` and `Debug.Assert` records to a buffer whose address
/// ShaderLibrary passes them with their arguments (MarlinLanguage.md §9.11). Each frame writes a buffer of its own;
/// once the GPU has finished the frame, BeginFrame reads the records and logs them, a failed assertion as an error.
MCLASS(Config = "Engine.Shaders", Dependencies = "RhiSubsystem, AssetSubsystem")
class SHADER_API ShaderLibrary final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Device profiles, best first; the converter compiles every one for its platform.
    MPROPERTY()
    static List<ShaderProfile> Profiles;
    /// 32-bit words of debug output a frame can hold; records beyond them are dropped and counted. Zero turns debug
    /// output off: shaders that print are passed no buffer and must not run.
    MPROPERTY()
    static uint32 DebugOutputWords;

    ShaderLibrary();
    ~ShaderLibrary() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// The profile the device runs, or null if none of Profiles does.
    [[nodiscard]] const ShaderProfile *GetProfile() const
    {
        return _profile;
    }

    /// Starts loading *shader* and making its pipeline, such as on a loading screen, before anything dispatches it.
    void Preload(const ShaderDeclaration &shader);
    /// True once *shader* has loaded and its pipeline is ready; starts loading it if nothing has asked for it yet.
    [[nodiscard]] bool IsReady(const ShaderDeclaration &shader);

    /// The pipeline of *shader* once it is ready, for passes that bind it themselves; else an invalid handle.
    [[nodiscard]] RhiPipeline GetPipeline(const ShaderDeclaration &shader);
    /// The graphics pipeline of *desc* once each of its stages has loaded and the pipeline is ready; else an invalid
    /// handle. Starts loading the stages nothing has asked for yet.
    [[nodiscard]] RhiPipeline GetPipeline(const ShaderGraphicsPipelineDesc &desc);
    /// The thread group size of *shader*'s entry point once it is ready; false before that.
    [[nodiscard]] bool GetGroupSize(const ShaderDeclaration &shader, uint32 (&size)[3]);

    /// Dispatches *x* by *y* by *z* groups of *shader* with *arguments*; false, recording nothing, until it is ready.
    template <typename TArgs>
    bool Dispatch(RhiCommandList &list, const ComputeShader<TArgs> &shader, const TArgs &arguments, uint32 x,
                  uint32 y = 1, uint32 z = 1)
    {
        return Dispatch(list, shader, &arguments, x, y, z);
    }

    /// The same for *x* by *y* by *z* threads, rounded up to whole groups.
    template <typename TArgs>
    bool DispatchThreads(RhiCommandList &list, const ComputeShader<TArgs> &shader, const TArgs &arguments, uint32 x,
                         uint32 y = 1, uint32 z = 1)
    {
        return DispatchThreads(list, shader, &arguments, x, y, z);
    }

    /// The same with the group counts read by the GPU from three uint32 at *offset* in *buffer*.
    template <typename TArgs>
    bool DispatchIndirect(RhiCommandList &list, const ComputeShader<TArgs> &shader, const TArgs &arguments,
                          RhiBuffer buffer, uint64 offset)
    {
        return DispatchIndirect(list, shader, &arguments, buffer, offset);
    }

    bool Dispatch(RhiCommandList &list, const ComputeShaderNoArguments &shader, uint32 x, uint32 y = 1, uint32 z = 1)
    {
        return Dispatch(list, shader, nullptr, x, y, z);
    }

    /// Draws *x* by *y* by *z* task (or mesh) groups of the pipeline of *desc* with *arguments*, inside a pass that
    /// renders; false, recording nothing, until the pipeline is ready. TArgs is the stages' [Uniform] parameter block.
    template <typename TArgs>
    bool DrawMesh(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const TArgs &arguments, uint32 x,
                  uint32 y = 1, uint32 z = 1)
    {
        return DrawMesh(list, desc, &arguments, sizeof(TArgs), x, y, z);
    }

    /// The same with up to *maxCount* group counts read by the GPU from three uint32 each at *offset* in *buffer*,
    /// and the count from a uint32 at *countOffset* in *countBuffer* if it is valid.
    template <typename TArgs>
    bool DrawMeshIndirect(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const TArgs &arguments,
                          RhiBuffer buffer, uint64 offset, uint32 maxCount = 1, RhiBuffer countBuffer = {},
                          uint64 countOffset = 0)
    {
        return DrawMeshIndirect(list, desc, &arguments, sizeof(TArgs), buffer, offset, maxCount, countBuffer,
                                countOffset);
    }

    /// Draws *draw* with the vertex pipeline of *desc*, such as a fullscreen triangle.
    template <typename TArgs>
    bool Draw(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const TArgs &arguments,
              const RhiDrawArguments &draw)
    {
        return Draw(list, desc, &arguments, sizeof(TArgs), draw);
    }

    /// The declaration of material pass *pass* compiled for *material* with *variant* (MarlinLanguage.md §9.4): it
    /// dispatches and draws like *pass* and takes its arguments, but loads its own GlobalShader. The library owns it;
    /// it stays valid until shutdown.
    template <typename TArgs>
    [[nodiscard]] const ComputeShader<TArgs> &GetMaterialShader(const ComputeShader<TArgs> &pass, StringView material,
                                                                Span<const ShaderVariantValue> variant = {})
    {
        return static_cast<const ComputeShader<TArgs> &>(
            GetDerivedShader(MakeMaterialShaderKey(pass.EntryPoint, material, variant),
                             [](StringView key) -> UniquePtr<ShaderDeclaration> {
                                 return MakeUnique<ComputeShader<TArgs>>(key);
                             }));
    }

    /// The declaration of *shader* with its `[Specialization]` constants set to *values* by name, the others at their
    /// defaults (MarlinLanguage.md §9.4): it loads *shader*'s GlobalShader and dispatches and draws like it, with a
    /// pipeline of its own. A name the shader lacks makes it unusable, with an error. The library owns it; it stays
    /// valid until shutdown.
    template <typename TShader>
    [[nodiscard]] const TShader &GetSpecializedShader(const TShader &shader,
                                                      Span<const ShaderSpecializationValue> values)
    {
        return static_cast<const TShader &>(
            GetDerivedShader(MakeSpecializedShaderKey(shader.EntryPoint, values),
                             [](StringView key) -> UniquePtr<ShaderDeclaration> { return MakeUnique<TShader>(key); }));
    }

    /// Finds the entry point of a Guid some declaration was asked for under, for stores that locate shaders by
    /// name; false for other Guids.
    [[nodiscard]] bool FindEntryPoint(const Guid &guid, String &entryPoint) const;

private:
    struct Entry;
    struct ReadyShader;
    /// A declaration the library made for a key, such as a material pass's or a specialized one's, of its source's
    /// declaration type, with the key its EntryPoint views.
    struct DerivedDeclaration
    {
        String Key;
        UniquePtr<ShaderDeclaration> Declaration;
    };
    using MakeDeclaration = UniquePtr<ShaderDeclaration> (*)(StringView key);
    const ShaderDeclaration &GetDerivedShader(String key, MakeDeclaration make);

    bool Dispatch(RhiCommandList &list, const ShaderDeclaration &shader, const void *arguments, uint32 x, uint32 y,
                  uint32 z);
    bool DispatchThreads(RhiCommandList &list, const ShaderDeclaration &shader, const void *arguments, uint32 x,
                         uint32 y, uint32 z);
    bool DispatchIndirect(RhiCommandList &list, const ShaderDeclaration &shader, const void *arguments,
                          RhiBuffer buffer, uint64 offset);
    bool BindGraphics(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const void *arguments,
                      uint32 size);
    bool DrawMesh(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const void *arguments, uint32 size,
                  uint32 x, uint32 y, uint32 z);
    bool DrawMeshIndirect(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const void *arguments,
                          uint32 size, RhiBuffer buffer, uint64 offset, uint32 maxCount, RhiBuffer countBuffer,
                          uint64 countOffset);
    bool Draw(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const void *arguments, uint32 size,
              const RhiDrawArguments &draw);
    /// The pipeline and group size of compute *shader* once it is ready, else null. Lock-free once the declaration's
    /// cache names a ready entry; Resolve otherwise.
    const ReadyShader *FindReady(const ShaderDeclaration &shader);
    /// The entry of compute *shader*, made on first use, and its ready state once its variant and pipeline are ready.
    const ReadyShader *Resolve(const ShaderDeclaration &shader);
    /// The entry of graphics stage *shader*, made on first use; *loaded* once its variant is usable. Under the lock.
    Entry *ResolveStage(const ShaderDeclaration &shader, bool &loaded);
    /// The entry of *uuid*, made if new, and the declaration's cache pointed at it; under the lock.
    Entry *FindEntry(const ShaderDeclaration &shader, const Guid &uuid);
    /// Takes a newly loaded *object* for the entry and, for a compute shader, fills the record of its pipeline; false
    /// if it has no pipeline of its own to make. Under the lock; Resolve gets the pipeline without it.
    bool StartPipeline(const ShaderDeclaration &shader, Entry &entry, const GlobalShader *object,
                       RhiPipelineRecord &record);
    /// The entry's ready state, made once its pipeline is ready; under the lock.
    const ReadyShader *GetReady(Entry &entry);
    const ReadyShader *BindPipeline(RhiCommandList &list, const ShaderDeclaration &shader, const void *arguments);
    /// How a pipeline's stages take their [Uniform] arguments.
    struct ArgumentLayout
    {
        /// The block is in upload memory and its address is passed instead (§9.3).
        bool Indirect = false;
        /// Where the debug output buffer's address goes, or ~0u (§9.11).
        uint32 DebugBufferOffset = ~0u;
    };
    struct GraphicsPipeline
    {
        RhiPipeline Pipeline;
        ArgumentLayout Layout;
    };
    /// A frame's debug output: the buffer the GPU appends records to, and when the work that wrote it is done.
    struct DebugOutput
    {
        RhiBuffer Buffer;
        uint32 *Words = nullptr;
        uint64 Address = 0;
        RhiQueueValues RetiredAt;
        bool Retired = false;
    };
    static constexpr uint32 DebugOutputCount = 4;

    /// Passes *size* bytes of [Uniform] arguments as *layout* says, with the debug output buffer's address.
    void SetArguments(RhiCommandList &list, const void *arguments, uint32 size, const ArgumentLayout &layout);
    /// The graphics pipeline of *desc* and its arguments' layout; an invalid pipeline until it is ready.
    [[nodiscard]] GraphicsPipeline GetGraphicsPipeline(const ShaderGraphicsPipelineDesc &desc);
    void CreateDebugOutputs();
    static void OnSubmitFrame(uint64 frame, void *context);
    /// At the start of the submitter's frame: retires the memory the last frame's commands used and reads its debug
    /// output once the GPU is done with it.
    void RetireFrame();
    /// Notes the formats *variant* prints, by their hash; under the lock.
    void AddDebugFormats(const ShaderVariant &variant);
    /// Logs the records of *output* and empties it; the GPU is done with it.
    void ReadDebugOutput(DebugOutput &output);
    bool CheckArguments(const ShaderDeclaration &shader, const ShaderVariant &variant) const;
    void OnReloaded(const Guid &guid);
    static RhiPipeline MakePipeline(RhiDevice &device, const RhiPipelineRecord &record, void *context);
    static RhiPipeline MakeGraphicsPipeline(RhiDevice &device, const ShaderLibrary &library,
                                            const RhiPipelineRecord &record);

    RhiSubsystem *_rhi = nullptr;
    RhiDevice *_device = nullptr;
    RhiPipelineLibrary *_pipelines = nullptr;
    /// Where [Uniform] blocks too large to pass directly go; retired each frame.
    UniquePtr<RhiUploadAllocator> _argumentUploads;
    const ShaderProfile *_profile = nullptr;
    /// What declaration caches name this library by while it has entries; never reused (ShaderDeclarationCache).
    Atomic<uint64> _generation{0};
    mutable Mutex _lock;
    HashMap<String, UniquePtr<DerivedDeclaration>> _derivedDeclarations;
    HashMap<Guid, UniquePtr<Entry>> _entries;
    /// Graphics pipelines by their key: the desc and the code of its stages, so a reloaded stage makes a new one.
    HashMap<Hash128, GraphicsPipeline> _graphics;
    DebugOutput _debugOutputs[DebugOutputCount];
    uint32 _debugOutput = 0;
    /// The address of the buffer this frame's work writes debug output to; zero when there is none.
    Atomic<uint64> _debugAddress{0};
    /// The formats of the loaded variants by MirDebugFormatHash, for reading debug output; under the lock.
    HashMap<uint32, String> _debugFormats;
    /// Entries asked for since the last frame, which BeginFrame starts loading on the main thread.
    List<Entry *> _pending;
};
} // namespace Marlin
