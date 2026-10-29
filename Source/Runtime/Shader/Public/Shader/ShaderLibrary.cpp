#include "Shader/ShaderLibrary.h"

#include "Asset/Assets.h"
#include "Diagnostics/AssertFormat.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiPipelineLibrary.h"
#include "RHI/RhiSubsystem.h"
#include "Reflection/TypeInfo.h"
#include "Shader/ShaderDebugOutput.h"

MARLIN_DEFINE_LOG_CATEGORY(LogShader);

namespace Marlin
{
List<ShaderProfile> ShaderLibrary::Profiles;
uint32 ShaderLibrary::DebugOutputWords = 1u << 16;

namespace
{
/// The stages of a graphics desc, in pipeline order.
constexpr uint32 GraphicsStageCount = 4;

/// What names a pipeline of *variant* run as *key*: its code, and the specialization values the key sets.
Hash128 GetPipelineCodeHash(const ShaderVariant &variant, StringView key)
{
    const usize hash = key.IndexOf('#');
    if (hash == StringView::NotFound)
    {
        return variant.GetCodeHash();
    }
    const Hash128 code = variant.GetCodeHash();
    String bytes = Format("{0:x}{1:x}{2}", code.Low, code.High, key.Substring(hash));
    return Xxh3Hash128(bytes.Data(), bytes.Length());
}

/// The specialization constants of *variant* run as *key*, dense by id: the key's values over the defaults. False,
/// with an error, if the key names a constant the variant lacks.
bool GetSpecializationConstants(const ShaderVariant &variant, StringView key, List<uint32> &constants)
{
    constants.Clear();
    for (const ShaderSpecialization &specialization : variant.Specializations)
    {
        if (specialization.Id >= constants.Count())
        {
            constants.Resize(specialization.Id + 1, 0u);
        }
        constants[specialization.Id] = specialization.DefaultBits;
    }
    List<ShaderSpecializationValue> values;
    if (!ParseShaderKeySpecializations(key, values))
    {
        MARLIN_LOG(LogShader, Error, "{0} has malformed specialization values", key);
        return false;
    }
    for (const ShaderSpecializationValue &value : values)
    {
        bool found = false;
        for (const ShaderSpecialization &specialization : variant.Specializations)
        {
            // By its qualified name, or by its own name after the namespace's.
            const StringView name = specialization.Name.AsView();
            const usize dot = name.LastIndexOf('.');
            if (name == value.Name.AsView() || (dot != StringView::NotFound && name.Substring(dot + 1) == value.Name.AsView()))
            {
                constants[specialization.Id] = value.Bits;
                found = true;
            }
        }
        if (!found)
        {
            MARLIN_LOG(LogShader, Error, "{0} has no specialization constant {1}", GetShaderKeyAsset(key), value.Name);
            return false;
        }
    }
    return true;
}

void GetStages(const ShaderGraphicsPipelineDesc &desc, const ShaderDeclaration *(&stages)[GraphicsStageCount])
{
    stages[0] = desc.Task;
    stages[1] = desc.Mesh;
    stages[2] = desc.Vertex;
    stages[3] = desc.Pixel;
}

/// Writes or reads the payload of a graphics pipeline record field by field: the entry points of its stages and its
/// state, which is all MakeGraphicsPipeline needs to make it again. Reading fails on a payload of another version.
class GraphicsPayload
{
public:
    explicit GraphicsPayload(List<uint8> &bytes) : _bytes(&bytes)
    {
    }

    explicit GraphicsPayload(Span<const uint8> bytes) : _source(bytes)
    {
    }

    void Write(const ShaderGraphicsPipelineDesc &desc)
    {
        const ShaderDeclaration *stages[GraphicsStageCount];
        GetStages(desc, stages);
        StringView entryPoints[GraphicsStageCount];
        for (uint32 index = 0; index < GraphicsStageCount; ++index)
        {
            entryPoints[index] = stages[index] != nullptr ? stages[index]->EntryPoint : StringView();
        }
        ShaderGraphicsPipelineDesc state = desc;
        Visit(entryPoints, state);
    }

    /// The entry points are views of the payload.
    [[nodiscard]] bool Read(StringView (&entryPoints)[GraphicsStageCount], ShaderGraphicsPipelineDesc &desc)
    {
        Visit(entryPoints, desc);
        return !_failed && _offset == _source.Count();
    }

private:
    static constexpr uint8 Version = 1;

    template <typename T> void Field(T &value)
    {
        static_assert(sizeof(T) <= 8, "payload fields are scalars");
        if (_bytes != nullptr)
        {
            uint8 raw[sizeof(T)];
            MemCopy(raw, &value, sizeof(T));
            _bytes->AddRange(Span<const uint8>(raw, sizeof(T)));
            return;
        }
        if (_offset + sizeof(T) > _source.Count())
        {
            _failed = true;
            return;
        }
        MemCopy(&value, _source.Data() + _offset, sizeof(T));
        _offset += sizeof(T);
    }

    void Field(StringView &text)
    {
        uint16 length = static_cast<uint16>(text.Length());
        Field(length);
        if (_bytes != nullptr)
        {
            _bytes->AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(text.Data()), length));
            return;
        }
        if (_failed || _offset + length > _source.Count())
        {
            _failed = true;
            return;
        }
        text = StringView(reinterpret_cast<const char *>(_source.Data() + _offset), length);
        _offset += length;
    }

    void Field(RhiStencilFace &face)
    {
        Field(face.Fail);
        Field(face.DepthFail);
        Field(face.Pass);
        Field(face.Compare);
    }

    void Visit(StringView (&entryPoints)[GraphicsStageCount], ShaderGraphicsPipelineDesc &desc)
    {
        uint8 version = Version;
        Field(version);
        _failed = _failed || version != Version;
        for (StringView &entryPoint : entryPoints)
        {
            Field(entryPoint);
        }
        Field(desc.Topology);
        Field(desc.Raster.Fill);
        Field(desc.Raster.Cull);
        Field(desc.Raster.FrontCounterClockwise);
        Field(desc.Raster.DepthClip);
        Field(desc.Raster.Conservative);
        Field(desc.Raster.DepthBias);
        Field(desc.Raster.DepthBiasClamp);
        Field(desc.Raster.DepthBiasSlope);
        RhiDepthStencilState &depth = desc.DepthStencil;
        Field(depth.DepthTest);
        Field(depth.DepthWrite);
        Field(depth.DepthCompare);
        Field(depth.StencilTest);
        Field(depth.StencilReadMask);
        Field(depth.StencilWriteMask);
        Field(depth.Front);
        Field(depth.Back);
        Field(depth.DepthBoundsTest);
        Field(desc.AlphaToCoverage);
        uint8 targets = static_cast<uint8>(Min(desc.RenderTargetCount, RhiMaxRenderTargets));
        Field(targets);
        _failed = _failed || targets > RhiMaxRenderTargets;
        desc.RenderTargetCount = _failed ? 0 : targets;
        for (uint32 index = 0; index < desc.RenderTargetCount; ++index)
        {
            RhiBlendTarget &blend = desc.Blend[index];
            Field(desc.RenderTargetFormats[index]);
            Field(blend.Enabled);
            Field(blend.SourceColor);
            Field(blend.DestColor);
            Field(blend.ColorOp);
            Field(blend.SourceAlpha);
            Field(blend.DestAlpha);
            Field(blend.AlphaOp);
            Field(blend.WriteMask);
        }
        Field(desc.DepthStencilFormat);
        Field(desc.SampleCount);
        Field(desc.ViewMask);
    }

    List<uint8> *_bytes = nullptr;
    Span<const uint8> _source;
    usize _offset = 0;
    bool _failed = false;
};

/// The key of a graphics pipeline: its payload and the code of each of its stages, so a stage that reloads with
/// other code names another pipeline.
Hash128 GetGraphicsKey(Span<const uint8> payload, Span<const Hash128> code)
{
    List<uint8> bytes;
    bytes.AddRange(payload);
    bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(code.Data()), code.Count() * sizeof(Hash128)));
    return Xxh3Hash128(bytes.Data(), bytes.Count());
}

Atomic<uint64> NextGeneration{1};

/// Distinct debug output messages a frame logs; the others are counted.
constexpr uint32 MaxDebugMessages = 32;

bool IsComplete(const RhiQueueValues &retiredAt, const RhiQueueValues &completed)
{
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        if (retiredAt.Values[queue] > completed.Values[queue])
        {
            return false;
        }
    }
    return true;
}
} // namespace

/// What a dispatch needs of a ready compute shader. A reload makes a new one; the old ones stay until Deinitialize,
/// so a dispatch that read one just before the reload still has a valid pipeline and group size.
struct ShaderLibrary::ReadyShader
{
    RhiPipeline Pipeline;
    uint32 GroupSize[3] = {1, 1, 1};
    ArgumentLayout Layout;
};

struct ShaderLibrary::Entry
{
    /// GetGlobalShaderGuid of the key, which may set specialization values; Asset is that of the shader it loads.
    Guid Uuid;
    Guid Asset;
    String EntryPoint;
    AssetRef<GlobalShader> Shader;
    AssetReloadId Reload = 0;
    const GlobalShader *Resolved = nullptr;
    const ShaderVariant *Variant = nullptr;
    RhiPipeline Pipeline;
    bool Started = false;
    bool Unusable = false;
    /// Set once a compute pipeline is ready, cleared when the shader reloads.
    Atomic<const ReadyShader *> Ready{nullptr};
    List<UniquePtr<ReadyShader>> ReadyShaders;
};

ShaderLibrary::ShaderLibrary() = default;
ShaderLibrary::~ShaderLibrary() = default;

void ShaderLibrary::Initialize(SubsystemContext &context)
{
    RhiSubsystem *rhi = context.Collection->Get<RhiSubsystem>();
    _device = rhi != nullptr ? rhi->GetDevice() : nullptr;
    _pipelines = rhi != nullptr ? rhi->GetPipelineLibrary() : nullptr;
    if (_device == nullptr || _pipelines == nullptr)
    {
        return;
    }
    const Name backend = _device->GetBackendName();
    for (const ShaderProfile &profile : Profiles)
    {
        if (Name(profile.Rhi.AsView()) != backend)
        {
            continue;
        }
        bool supported = true;
        for (const String &feature : profile.Features)
        {
            supported = supported && _device->HasFeature(feature.AsView());
        }
        if (supported)
        {
            _profile = &profile;
            break;
        }
    }
    if (_profile == nullptr)
    {
        MARLIN_LOG(LogShader, Error, "No shader profile of Engine.Shaders.Profiles runs on this {0} device", backend);
        return;
    }
    _argumentUploads = MakeUnique<RhiUploadAllocator>(*_device, uint64{256} * 1024);
    CreateDebugOutputs();
    _rhi = rhi;
    _rhi->AddSubmitFrameCallback(&ShaderLibrary::OnSubmitFrame, this);
    _generation.Store(NextGeneration.FetchAdd(1));
    MARLIN_LOG(LogShader, Info, "Shader profile {0}", _profile->Name);
}

void ShaderLibrary::Deinitialize()
{
    if (_rhi != nullptr)
    {
        _rhi->RemoveSubmitFrameCallback(&ShaderLibrary::OnSubmitFrame, this);
        _rhi = nullptr;
    }
    const ScopeLock lock(_lock);
    // Declaration caches naming this library's entries no longer match.
    _generation.Store(0);
    for (auto &pair : _entries)
    {
        if (pair.Value->Reload != 0)
        {
            Assets::UnwatchReload(pair.Value->Reload);
        }
    }
    _entries.Clear();
    _pending.Clear();
    _profile = nullptr;
    _argumentUploads.Reset();
    _debugAddress.Store(0);
    for (DebugOutput &output : _debugOutputs)
    {
        if (output.Buffer.IsValid())
        {
            _device->Destroy(output.Buffer);
        }
        output = DebugOutput();
    }
    _debugFormats.Clear();
}

void ShaderLibrary::CreateDebugOutputs()
{
    if (DebugOutputWords == 0)
    {
        return;
    }
    // GPU-written and CPU-read: the records are read once the frame is done, without a copy.
    RhiBufferDesc desc;
    desc.Size = (uint64{ShaderDebugHeaderWords} + DebugOutputWords + ShaderDebugScratchWords) * sizeof(uint32);
    desc.Usage = RhiBufferUsage::ShaderResource | RhiBufferUsage::UnorderedAccess;
    desc.Location = RhiMemoryLocation::Readback;
    desc.DebugName = "Shader Debug Output";
    for (DebugOutput &output : _debugOutputs)
    {
        Result<RhiBuffer, RhiError> buffer = _device->CreateBuffer(desc);
        if (buffer.IsError())
        {
            MARLIN_LOG(LogShader, Error, "The shader debug output buffers could not be created: {0}",
                       GetErrorName(buffer.Error()));
            return;
        }
        output.Buffer = buffer.Value();
        output.Words = reinterpret_cast<uint32 *>(_device->GetMappedData(output.Buffer));
        output.Address = _device->GetBufferAddress(output.Buffer);
        output.Words[ShaderDebugCount] = 0;
        output.Words[ShaderDebugCapacity] = DebugOutputWords;
    }
    _debugOutput = 0;
    _debugAddress.Store(_debugOutputs[0].Address, MemoryOrder::Release);
}

void ShaderLibrary::AddDebugFormats(const ShaderVariant &variant)
{
    for (const FrozenString &format : variant.DebugFormats)
    {
        _debugFormats.FindOrAdd(GetShaderDebugFormatHash(format.AsView())) = String(format.AsView());
    }
}

void ShaderLibrary::ReadDebugOutput(DebugOutput &output)
{
    List<ShaderDebugMessage> messages;
    uint32 dropped = 0;
    {
        const ScopeLock lock(_lock);
        const usize words = ShaderDebugHeaderWords + DebugOutputWords;
        dropped = ReadShaderDebugOutput(Span<const uint32>(output.Words, words), _debugFormats, messages);
    }
    output.Words[ShaderDebugCount] = 0;
    // Threads that fail alike print alike: each message once, with how often it came.
    List<Pair<const ShaderDebugMessage *, uint32>> distinct;
    HashMap<StringView, uint32> indices;
    for (const ShaderDebugMessage &message : messages)
    {
        if (const uint32 *index = indices.Find(message.Text.AsView()))
        {
            ++distinct[*index].Second;
            continue;
        }
        indices.Add(message.Text.AsView(), static_cast<uint32>(distinct.Count()));
        distinct.Add({&message, 1u});
    }
    for (usize i = 0; i < distinct.Count() && i < MaxDebugMessages; ++i)
    {
        const auto &[message, count] = distinct[i];
        StringBuilder<64> times;
        if (count > 1)
        {
            FormatTo(times, " ({0} times)", count);
        }
        if (message->Assertion)
        {
            MARLIN_LOG(LogShader, Error, "{0}{1}", message->Text, times.AsView());
        }
        else
        {
            MARLIN_LOG(LogShader, Info, "{0}{1}", message->Text, times.AsView());
        }
    }
    if (distinct.Count() > MaxDebugMessages)
    {
        MARLIN_LOG(LogShader, Warning, "{0} more distinct shader debug messages this frame",
                   distinct.Count() - MaxDebugMessages);
    }
    if (dropped > 0)
    {
        MARLIN_LOG(LogShader, Warning,
                   "{0} words of shader debug output did not fit; Engine.Shaders.DebugOutputWords is {1}", dropped,
                   DebugOutputWords);
    }
}

void ShaderLibrary::OnSubmitFrame(uint64 /*frame*/, void *context)
{
    static_cast<ShaderLibrary *>(context)->RetireFrame();
}

void ShaderLibrary::RetireFrame()
{
    // The submitter's last frame is all submitted: what it used retires with its work.
    if (_argumentUploads != nullptr)
    {
        // The [Uniform] blocks the frame uploaded live until its work is done (§9.3).
        _argumentUploads->EndFrame();
    }
    if (_debugOutputs[_debugOutput].Words != nullptr)
    {
        // What is done is read. The next frame writes the next buffer, waiting for the GPU if the frame
        // that wrote it last is still running.
        DebugOutput &written = _debugOutputs[_debugOutput];
        written.RetiredAt = _device->GetSubmittedValues();
        written.Retired = true;
        const RhiQueueValues completed = _device->GetCompletedValues();
        for (DebugOutput &output : _debugOutputs)
        {
            if (output.Retired && IsComplete(output.RetiredAt, completed))
            {
                output.Retired = false;
                ReadDebugOutput(output);
            }
        }
        _debugOutput = (_debugOutput + 1) % DebugOutputCount;
        DebugOutput &next = _debugOutputs[_debugOutput];
        if (next.Retired)
        {
            for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
            {
                if (next.RetiredAt.Values[queue] > completed.Values[queue])
                {
                    const auto type = static_cast<RhiQueueType>(queue);
                    (void)_device->Wait(_device->GetQueueFence(type), next.RetiredAt.Values[queue]);
                }
            }
            next.Retired = false;
            ReadDebugOutput(next);
        }
        _debugAddress.Store(next.Address, MemoryOrder::Release);
    }
}

void ShaderLibrary::BeginFrame(const FrameTime & /*time*/)
{
    List<Entry *> pending;
    {
        const ScopeLock lock(_lock);
        pending = Move(_pending);
        _pending.Clear();
    }
    for (Entry *entry : pending)
    {
        AssetRef<GlobalShader> shader = Assets::Load<GlobalShader>(entry->Asset, IoPriority::Critical);
        AssetReloadId reload = 0;
        if (AssetSlot *slot = shader.GetSlot())
        {
            const Guid uuid = entry->Uuid;
            reload = Assets::WatchReload(*slot, [this, uuid](AssetSlot &) { OnReloaded(uuid); });
        }
        const ScopeLock lock(_lock);
        entry->Shader = Move(shader);
        entry->Reload = reload;
    }
}

void ShaderLibrary::OnReloaded(const Guid &guid)
{
    const ScopeLock lock(_lock);
    if (const UniquePtr<Entry> *entry = _entries.Find(guid))
    {
        // Dispatches take the locked path again, which makes the new pipeline.
        (*entry)->Ready.Store(nullptr, MemoryOrder::Release);
        MARLIN_LOG(LogShader, Info, "Reloaded {0}", (*entry)->EntryPoint);
    }
}

void ShaderLibrary::Preload(const ShaderDeclaration &shader)
{
    (void)IsReady(shader);
}

bool ShaderLibrary::IsReady(const ShaderDeclaration &shader)
{
    if (shader.Stage != ShaderStage::Compute)
    {
        // A graphics stage is ready once it has loaded; the pipelines are its descs' (GetPipeline).
        bool loaded = false;
        const ScopeLock lock(_lock);
        (void)ResolveStage(shader, loaded);
        return loaded;
    }
    return FindReady(shader) != nullptr;
}

RhiPipeline ShaderLibrary::GetPipeline(const ShaderDeclaration &shader)
{
    if (shader.Stage != ShaderStage::Compute)
    {
        (void)IsReady(shader);
        return {};
    }
    const ReadyShader *ready = FindReady(shader);
    return ready != nullptr ? ready->Pipeline : RhiPipeline();
}

bool ShaderLibrary::GetGroupSize(const ShaderDeclaration &shader, uint32 (&size)[3])
{
    if (shader.Stage != ShaderStage::Compute)
    {
        bool loaded = false;
        const ScopeLock lock(_lock);
        const Entry *entry = ResolveStage(shader, loaded);
        if (!loaded)
        {
            return false;
        }
        size[0] = entry->Variant->GroupSizeX;
        size[1] = entry->Variant->GroupSizeY;
        size[2] = entry->Variant->GroupSizeZ;
        return true;
    }
    const ReadyShader *ready = FindReady(shader);
    if (ready == nullptr)
    {
        return false;
    }
    size[0] = ready->GroupSize[0];
    size[1] = ready->GroupSize[1];
    size[2] = ready->GroupSize[2];
    return true;
}

const ShaderDeclaration &ShaderLibrary::GetDerivedShader(String key, MakeDeclaration make)
{
    const ScopeLock lock(_lock);
    if (const UniquePtr<DerivedDeclaration> *found = _derivedDeclarations.Find(key))
    {
        return *(*found)->Declaration;
    }
    UniquePtr<DerivedDeclaration> created = MakeUnique<DerivedDeclaration>();
    created->Key = Move(key);
    created->Declaration = make(created->Key.AsView());
    const ShaderDeclaration &declaration = *created->Declaration;
    // The key is copied before the entry moves into the map: the arguments' order is unspecified.
    String mapKey = created->Key;
    _derivedDeclarations.Add(Move(mapKey), Move(created));
    return declaration;
}

bool ShaderLibrary::FindEntryPoint(const Guid &guid, String &entryPoint) const
{
    const ScopeLock lock(_lock);
    if (const UniquePtr<Entry> *entry = _entries.Find(guid))
    {
        entryPoint = String(GetShaderKeyAsset((*entry)->EntryPoint.AsView()));
        return true;
    }
    // A shader only specialized declarations have asked for.
    for (const auto &[uuid, entry] : _entries)
    {
        if (entry->Asset == guid)
        {
            entryPoint = String(GetShaderKeyAsset(entry->EntryPoint.AsView()));
            return true;
        }
    }
    return false;
}

const ShaderLibrary::ReadyShader *ShaderLibrary::FindReady(const ShaderDeclaration &shader)
{
    // Resolve writes the cache as generation 0, entry, generation: the same live generation read on both sides of
    // the entry means the entry is this library's for this declaration.
    const uint64 generation = _generation.Load(MemoryOrder::Relaxed);
    if (generation != 0 && shader.Cache.Generation.Load(MemoryOrder::Acquire) == generation)
    {
        const auto *entry = static_cast<const Entry *>(shader.Cache.Entry.Load(MemoryOrder::Acquire));
        if (entry != nullptr && shader.Cache.Generation.Load(MemoryOrder::Relaxed) == generation)
        {
            if (const ReadyShader *ready = entry->Ready.Load(MemoryOrder::Acquire))
            {
                return ready;
            }
        }
    }
    return Resolve(shader);
}

const ShaderLibrary::ReadyShader *ShaderLibrary::Resolve(const ShaderDeclaration &shader)
{
    if (_profile == nullptr)
    {
        return nullptr;
    }
    const Guid uuid = GetGlobalShaderGuid(shader.EntryPoint);
    Entry *entry = nullptr;
    const GlobalShader *object = nullptr;
    const ShaderVariant *variant = nullptr;
    RhiPipelineRecord record;
    {
        const ScopeLock lock(_lock);
        entry = FindEntry(shader, uuid);
        if (!entry->Started)
        {
            entry->Started = true;
            _pending.Add(entry);
            return nullptr;
        }
        object = entry->Shader.Get();
        if (object == nullptr)
        {
            return nullptr;
        }
        if (object == entry->Resolved || !StartPipeline(shader, *entry, object, record))
        {
            return GetReady(*entry);
        }
        variant = entry->Variant;
    }
    // Made outside the lock: creating a pipeline hands its SPIR-V to the crash tools, which hash it.
    const RhiPipeline pipeline = _pipelines->Get(record, &MakePipeline, const_cast<ShaderVariant *>(variant));
    const ScopeLock lock(_lock);
    if (entry->Resolved == object)
    {
        entry->Pipeline = pipeline;
    }
    return GetReady(*entry);
}

ShaderLibrary::Entry *ShaderLibrary::ResolveStage(const ShaderDeclaration &shader, bool &loaded)
{
    loaded = false;
    if (_profile == nullptr)
    {
        return nullptr;
    }
    Entry *entry = FindEntry(shader, GetGlobalShaderGuid(shader.EntryPoint));
    if (!entry->Started)
    {
        entry->Started = true;
        _pending.Add(entry);
        return entry;
    }
    const GlobalShader *object = entry->Shader.Get();
    if (object == nullptr)
    {
        return entry;
    }
    if (object != entry->Resolved)
    {
        RhiPipelineRecord record;
        (void)StartPipeline(shader, *entry, object, record);
    }
    loaded = !entry->Unusable;
    return entry;
}

ShaderLibrary::Entry *ShaderLibrary::FindEntry(const ShaderDeclaration &shader, const Guid &uuid)
{
    UniquePtr<Entry> &slot = _entries.FindOrAdd(uuid);
    if (slot == nullptr)
    {
        slot = MakeUnique<Entry>();
        slot->Uuid = uuid;
        slot->Asset = GetGlobalShaderGuid(GetShaderKeyAsset(shader.EntryPoint));
        slot->EntryPoint = String(shader.EntryPoint);
    }
    Entry *entry = slot.Get();
    const uint64 generation = _generation.Load(MemoryOrder::Relaxed);
    if (shader.Cache.Generation.Load() != generation || shader.Cache.Entry.Load() != entry)
    {
        shader.Cache.Generation.Store(0);
        shader.Cache.Entry.Store(entry);
        shader.Cache.Generation.Store(generation);
    }
    return entry;
}

bool ShaderLibrary::StartPipeline(const ShaderDeclaration &shader, Entry &entry, const GlobalShader *object,
                                  RhiPipelineRecord &record)
{
    entry.Ready.Store(nullptr, MemoryOrder::Release);
    entry.Resolved = object;
    entry.Pipeline = {};
    entry.Variant = object->FindVariant(_profile->Name.AsView());
    if (entry.Variant == nullptr)
    {
        MARLIN_LOG(LogShader, Error, "{0} has no variant for profile {1}", entry.EntryPoint, _profile->Name);
    }
    else if (entry.Variant->Stage != shader.Stage)
    {
        MARLIN_LOG(LogShader, Error, "{0} is declared with another stage than its entry point's", entry.EntryPoint);
    }
    List<uint32> constants;
    entry.Unusable = entry.Variant == nullptr || entry.Variant->Stage != shader.Stage ||
                     !CheckArguments(shader, *entry.Variant) ||
                     !GetSpecializationConstants(*entry.Variant, entry.EntryPoint.AsView(), constants);
    if (!entry.Unusable)
    {
        AddDebugFormats(*entry.Variant);
    }
    // A graphics stage has no pipeline of its own; its descs' are made by GetPipeline.
    if (entry.Unusable || shader.Stage != ShaderStage::Compute)
    {
        return false;
    }
    record.Key.Hash = GetPipelineCodeHash(*entry.Variant, entry.EntryPoint.AsView());
    record.Key.Kind = RhiPipelineKind::Compute;
    const StringView name = entry.EntryPoint.AsView();
    record.Payload.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(name.Data()), name.Length()));
    return true;
}

const ShaderLibrary::ReadyShader *ShaderLibrary::GetReady(Entry &entry)
{
    if (entry.Unusable || !entry.Pipeline.IsValid())
    {
        return nullptr;
    }
    if (const ReadyShader *ready = entry.Ready.Load(MemoryOrder::Relaxed))
    {
        return ready;
    }
    if (_device->GetPipelineState(entry.Pipeline) != RhiPipelineState::Ready)
    {
        return nullptr;
    }
    UniquePtr<ReadyShader> ready = MakeUnique<ReadyShader>();
    ready->Pipeline = entry.Pipeline;
    ready->GroupSize[0] = entry.Variant->GroupSizeX;
    ready->GroupSize[1] = entry.Variant->GroupSizeY;
    ready->GroupSize[2] = entry.Variant->GroupSizeZ;
    ready->Layout.Indirect = entry.Variant->ArgumentsIndirect;
    ready->Layout.DebugBufferOffset = entry.Variant->DebugBufferOffset;
    const ReadyShader *published = ready.Get();
    entry.ReadyShaders.Add(Move(ready));
    entry.Ready.Store(published, MemoryOrder::Release);
    return published;
}

RhiPipeline ShaderLibrary::GetPipeline(const ShaderGraphicsPipelineDesc &desc)
{
    return GetGraphicsPipeline(desc).Pipeline;
}

ShaderLibrary::GraphicsPipeline ShaderLibrary::GetGraphicsPipeline(const ShaderGraphicsPipelineDesc &desc)
{
    MARLIN_ASSERTF((desc.Mesh != nullptr) != (desc.Vertex != nullptr), "A graphics pipeline has a Mesh or a Vertex stage");
    MARLIN_ASSERTF(desc.Task == nullptr || desc.Mesh != nullptr, "A Task stage needs a Mesh stage");
    const ShaderDeclaration *stages[GraphicsStageCount];
    GetStages(desc, stages);
    RhiPipelineRecord record;
    GraphicsPayload(record.Payload).Write(desc);
    record.Key.Kind = RhiPipelineKind::Graphics;
    const ScopeLock lock(_lock);
    bool allReady = true;
    const Entry *entries[GraphicsStageCount] = {};
    for (uint32 index = 0; index < GraphicsStageCount; ++index)
    {
        if (stages[index] != nullptr)
        {
            bool loaded = false;
            entries[index] = ResolveStage(*stages[index], loaded);
            allReady = allReady && loaded;
        }
    }
    if (!allReady)
    {
        return {};
    }
    Hash128 code[GraphicsStageCount];
    uint32 count = 0;
    ArgumentLayout layout;
    for (const Entry *entry : entries)
    {
        if (entry != nullptr)
        {
            if (entry->Variant == nullptr)
            {
                return {};
            }
            code[count++] = GetPipelineCodeHash(*entry->Variant, entry->EntryPoint.AsView());
            // The stages compiled alike pass their shared arguments alike; those that print agree on where.
            layout.Indirect = layout.Indirect || entry->Variant->ArgumentsIndirect;
            if (entry->Variant->DebugBufferOffset != ~0u)
            {
                layout.DebugBufferOffset = entry->Variant->DebugBufferOffset;
            }
        }
    }
    record.Key.Hash = GetGraphicsKey(record.Payload.AsSpan(), Span<const Hash128>(code, count));
    GraphicsPipeline &pipeline = _graphics.FindOrAdd(record.Key.Hash);
    if (!pipeline.Pipeline.IsValid())
    {
        pipeline.Pipeline = _pipelines->Get(record, &MakePipeline, this);
        pipeline.Layout = layout;
    }
    if (!pipeline.Pipeline.IsValid() || _device->GetPipelineState(pipeline.Pipeline) != RhiPipelineState::Ready)
    {
        return {};
    }
    return pipeline;
}

RhiPipeline ShaderLibrary::MakePipeline(RhiDevice &device, const RhiPipelineRecord &record, void *context)
{
    // Called by RhiPipelineLibrary::Get. For a graphics record, from GetPipeline with the lock held, and *context* is
    // the library. For a compute record, from Resolve without the lock, and *context* is the entry's variant, which
    // stays loaded while the entry holds its shader.
    if (record.Key.Kind == RhiPipelineKind::Graphics)
    {
        return MakeGraphicsPipeline(device, *static_cast<const ShaderLibrary *>(context), record);
    }
    const auto *variantOf = static_cast<const ShaderVariant *>(context);
    const StringView name(reinterpret_cast<const char *>(record.Payload.Data()), record.Payload.Count());
    List<uint32> constants;
    if (variantOf == nullptr || GetPipelineCodeHash(*variantOf, name) != record.Key.Hash ||
        !GetSpecializationConstants(*variantOf, name, constants))
    {
        return {};
    }
    const ShaderVariant &variant = *variantOf;
    RhiComputePipelineDesc desc;
    desc.Shader.Code = variant.Code.AsSpan();
    desc.Shader.Stage = ToRhiShaderStage(variant.Stage);
    desc.Shader.SpecializationConstants = constants.AsSpan();
    // RECOVERY-DECISION D5: the SPIR-V backend names the entry point by its qualified name, not "main".
    desc.Shader.EntryPoint = GetShaderKeyEntryPoint(name);
    desc.DebugName = name;
    return device.CreatePipeline(desc);
}

RhiPipeline ShaderLibrary::MakeGraphicsPipeline(RhiDevice &device, const ShaderLibrary &library,
                                                const RhiPipelineRecord &record)
{
    StringView entryPoints[GraphicsStageCount];
    ShaderGraphicsPipelineDesc state;
    if (!GraphicsPayload(record.Payload.AsSpan()).Read(entryPoints, state))
    {
        return {};
    }
    RhiShader shaders[GraphicsStageCount];
    List<uint32> constants[GraphicsStageCount];
    Hash128 code[GraphicsStageCount];
    uint32 count = 0;
    for (const StringView entryPoint : entryPoints)
    {
        if (entryPoint.IsEmpty())
        {
            continue;
        }
        const UniquePtr<Entry> *entry = library._entries.Find(GetGlobalShaderGuid(entryPoint));
        if (entry == nullptr || (*entry)->Variant == nullptr)
        {
            return {};
        }
        const ShaderVariant &variant = *(*entry)->Variant;
        if (!GetSpecializationConstants(variant, entryPoint, constants[count]))
        {
            return {};
        }
        RhiShader &shader = shaders[count];
        shader.Code = variant.Code.AsSpan();
        shader.Stage = ToRhiShaderStage(variant.Stage);
        shader.EntryPoint = GetShaderKeyEntryPoint(entryPoint);
        shader.SpecializationConstants = constants[count].AsSpan();
        code[count++] = GetPipelineCodeHash(variant, entryPoint);
    }
    // A record of other code than the stages have now, such as one from an older pipeline list.
    if (GetGraphicsKey(record.Payload.AsSpan(), Span<const Hash128>(code, count)) != record.Key.Hash)
    {
        return {};
    }
    RhiGraphicsPipelineDesc desc;
    desc.Shaders = Span<const RhiShader>(shaders, count);
    desc.Topology = state.Topology;
    desc.Raster = state.Raster;
    desc.DepthStencil = state.DepthStencil;
    for (uint32 index = 0; index < RhiMaxRenderTargets; ++index)
    {
        desc.Blend[index] = state.Blend[index];
    }
    desc.AlphaToCoverage = state.AlphaToCoverage;
    desc.RenderTargetFormats = Span<const RhiFormat>(state.RenderTargetFormats, state.RenderTargetCount);
    desc.DepthStencilFormat = state.DepthStencilFormat;
    desc.SampleCount = state.SampleCount;
    desc.ViewMask = state.ViewMask;
    // Named by the stage that shapes it: the Mesh or the Vertex entry point.
    desc.DebugName = !entryPoints[1].IsEmpty() ? entryPoints[1] : entryPoints[2];
    return device.CreatePipeline(desc);
}

void ShaderLibrary::SetArguments(RhiCommandList &list, const void *arguments, uint32 size,
                                 const ArgumentLayout &layout)
{
    // What is passed directly, written in one piece: the arguments or, when they are more, the address of a copy
    // in upload memory (MarlinLanguage.md §9.3), then the debug output buffer's address where the shader prints
    // (§9.11), the padding before it zero.
    uint8 direct[RhiShaderArgumentsSize] = {};
    uint32 directSize = 0;
    if (size != 0 && !layout.Indirect)
    {
        MemCopy(direct, arguments, size);
        directSize = size;
    }
    else if (size != 0)
    {
        const RhiUploadAllocation upload = _argumentUploads->Allocate(size, 16);
        if (upload.Data != nullptr)
        {
            MemCopy(upload.Data, arguments, size);
        }
        else
        {
            MARLIN_LOG(LogShader, Error, "No upload memory for {0} bytes of [Uniform] arguments", size);
        }
        MemCopy(direct, &upload.Address, sizeof(uint64));
        directSize = sizeof(uint64);
    }
    if (layout.DebugBufferOffset != ~0u)
    {
        // A shader that prints with no buffer to print to would write to address zero.
        const uint64 address = _debugAddress.Load(MemoryOrder::Acquire);
        MARLIN_ASSERTF(address != 0, "A shader prints debug output while Engine.Shaders.DebugOutputWords is zero");
        MemCopy(direct + layout.DebugBufferOffset, &address, sizeof(uint64));
        directSize = Max(directSize, layout.DebugBufferOffset + static_cast<uint32>(sizeof(uint64)));
    }
    if (directSize != 0)
    {
        list.SetShaderArguments(Span<const uint8>(direct, directSize));
    }
}

bool ShaderLibrary::CheckArguments(const ShaderDeclaration &shader, const ShaderVariant &variant) const
{
    if (shader.Stage != ShaderStage::Compute && variant.ArgumentsSize == 0)
    {
        // A graphics stage that reads none of its pipeline's [Uniform] arguments.
        return true;
    }
    if (shader.Arguments == nullptr)
    {
        if (variant.ArgumentsSize != 0)
        {
            MARLIN_LOG(LogShader, Error, "{0} takes [Uniform] arguments its declaration does not pass", shader.EntryPoint);
            return false;
        }
        return true;
    }
    const TypeInfo &type = shader.Arguments();
    // Whether the block is passed directly is the compiler's choice: with debug output it keeps room for the debug
    // buffer's address (§9.11).
    bool matches = variant.ArgumentsSize == shader.ArgumentsSize;
    usize count = 0;
    for (const FieldInfo *field : type.GetFields())
    {
        if (field->IsStatic())
        {
            continue;
        }
        bool found = false;
        for (const ShaderArgumentField &constant : variant.Arguments)
        {
            if (constant.Name.AsView() == field->GetName())
            {
                found = constant.Offset == field->GetOffset() && constant.Size == field->GetType().GetSize();
                break;
            }
        }
        matches = matches && found;
        ++count;
    }
    matches = matches && count == variant.Arguments.Count();
    if (!matches)
    {
        MARLIN_LOG(LogShader, Error,
                   "{0}: {1} does not match the entry point's [Uniform] parameter block by name, offset and size ({2} bytes)",
                   shader.EntryPoint, type.GetName(), variant.ArgumentsSize);
    }
    return matches;
}

const ShaderLibrary::ReadyShader *ShaderLibrary::BindPipeline(RhiCommandList &list, const ShaderDeclaration &shader,
                                                              const void *arguments)
{
    const ReadyShader *ready = FindReady(shader);
    if (ready == nullptr)
    {
        return nullptr;
    }
    list.SetPipeline(ready->Pipeline);
    SetArguments(list, arguments, shader.ArgumentsSize, ready->Layout);
    return ready;
}

bool ShaderLibrary::Dispatch(RhiCommandList &list, const ShaderDeclaration &shader, const void *arguments, uint32 x,
                             uint32 y, uint32 z)
{
    if (BindPipeline(list, shader, arguments) == nullptr)
    {
        return false;
    }
    if (x != 0 && y != 0 && z != 0)
    {
        list.Dispatch(x, y, z);
    }
    return true;
}

bool ShaderLibrary::DispatchThreads(RhiCommandList &list, const ShaderDeclaration &shader, const void *arguments,
                                    uint32 x, uint32 y, uint32 z)
{
    const ReadyShader *ready = BindPipeline(list, shader, arguments);
    if (ready == nullptr)
    {
        return false;
    }
    const uint32 groupsX = (x + ready->GroupSize[0] - 1) / ready->GroupSize[0];
    const uint32 groupsY = (y + ready->GroupSize[1] - 1) / ready->GroupSize[1];
    const uint32 groupsZ = (z + ready->GroupSize[2] - 1) / ready->GroupSize[2];
    if (groupsX != 0 && groupsY != 0 && groupsZ != 0)
    {
        list.Dispatch(groupsX, groupsY, groupsZ);
    }
    return true;
}

bool ShaderLibrary::DispatchIndirect(RhiCommandList &list, const ShaderDeclaration &shader, const void *arguments,
                                     RhiBuffer buffer, uint64 offset)
{
    if (BindPipeline(list, shader, arguments) == nullptr)
    {
        return false;
    }
    list.ExecuteIndirect(_device->GetCommandSignature(RhiIndirectCommandKind::Dispatch), buffer, offset, 1);
    return true;
}

bool ShaderLibrary::BindGraphics(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const void *arguments,
                                 uint32 size)
{
    const ShaderDeclaration *stages[GraphicsStageCount];
    GetStages(desc, stages);
    for ([[maybe_unused]] const ShaderDeclaration *stage : stages)
    {
        MARLIN_ASSERTF(stage == nullptr || stage->ArgumentsSize == 0 || stage->ArgumentsSize == size,
                       "{0} takes other [Uniform] arguments than the draw passes", stage->EntryPoint);
    }
    const GraphicsPipeline pipeline = GetGraphicsPipeline(desc);
    if (!pipeline.Pipeline.IsValid())
    {
        return false;
    }
    list.SetPipeline(pipeline.Pipeline);
    SetArguments(list, arguments, size, pipeline.Layout);
    return true;
}

bool ShaderLibrary::DrawMesh(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const void *arguments,
                             uint32 size, uint32 x, uint32 y, uint32 z)
{
    if (!BindGraphics(list, desc, arguments, size))
    {
        return false;
    }
    if (x != 0 && y != 0 && z != 0)
    {
        list.DrawMesh(x, y, z);
    }
    return true;
}

bool ShaderLibrary::DrawMeshIndirect(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc,
                                     const void *arguments, uint32 size, RhiBuffer buffer, uint64 offset,
                                     uint32 maxCount, RhiBuffer countBuffer, uint64 countOffset)
{
    if (!BindGraphics(list, desc, arguments, size))
    {
        return false;
    }
    list.ExecuteIndirect(_device->GetCommandSignature(RhiIndirectCommandKind::DrawMesh), buffer, offset, maxCount,
                         countBuffer, countOffset);
    return true;
}

bool ShaderLibrary::Draw(RhiCommandList &list, const ShaderGraphicsPipelineDesc &desc, const void *arguments,
                         uint32 size, const RhiDrawArguments &draw)
{
    if (!BindGraphics(list, desc, arguments, size))
    {
        return false;
    }
    if (draw.VertexCount != 0 && draw.InstanceCount != 0)
    {
        list.Draw(draw);
    }
    return true;
}
} // namespace Marlin
