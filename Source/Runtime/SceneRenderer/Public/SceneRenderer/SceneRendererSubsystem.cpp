#include "SceneRenderer/SceneRendererSubsystem.h"

#include "Asset/AssetSubsystem.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "Diagnostics/AssertFormat.h"
#include "Engine/Engine.h"
#include "Math/MathUtility.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiDevice.h"
#include "Reflection/Reflection.h"
#include "RenderComponents/RenderComponents.h"
#include "Renderer/RendererSubsystem.h"
#include "SceneRenderer/RenderScene.h"
#include "SceneRenderer/SceneTextureResidency.h"
#include "Shader/ShaderLibrary.h"
#include "Templates/Bit.h"

namespace Marlin
{
uint64 SceneRendererSubsystem::StreamingPoolSize = uint64{512} << 20;
uint32 SceneRendererSubsystem::RootPages = 2048;
uint32 SceneRendererSubsystem::SlotSize = 128u << 10;
uint32 SceneRendererSubsystem::MaxMeshes = 1u << 16;
uint32 SceneRendererSubsystem::MaxNodes = 1u << 21;
uint32 SceneRendererSubsystem::MaxGroups = 1u << 21;
uint32 SceneRendererSubsystem::MaxPages = 1u << 20;
uint32 SceneRendererSubsystem::MaxSections = 1u << 18;
float32 SceneRendererSubsystem::Exposure = 14.5f;
float32 SceneRendererSubsystem::SunIlluminance = 100000;
float32 SceneRendererSubsystem::AmbientIlluminance = 20000;
float32 SceneRendererSubsystem::SunDirection[3] = {0.35f, 0.8f, 0.45f};
bool SceneRendererSubsystem::TextureStreaming = true;
uint32 SceneRendererSubsystem::TexturePoolSize = 1024;
float32 SceneRendererSubsystem::TextureMipBias = 0;
uint32 SceneRendererSubsystem::MaxAnisotropy = 16;
bool SceneRendererSubsystem::MaterialTileClasses = true;
uint32 SceneRendererSubsystem::MaxVisibleClusters = 1u << 22;
uint32 SceneRendererSubsystem::MaxVisibleGroups = 1u << 18;
uint32 SceneRendererSubsystem::TraversalQueueSize = 1u << 22;
uint32 SceneRendererSubsystem::TraversalWorkgroups = 512;
float32 SceneRendererSubsystem::LodPixelError = 1.0f;
float32 SceneRendererSubsystem::SoftwareRasterPixels = 32;
uint32 SceneRendererSubsystem::MaxPageRequests = 4096;
uint32 SceneRendererSubsystem::MaxPageLoads = 128;
uint32 SceneRendererSubsystem::MaxPageCommits = 128;
float32 SceneRendererSubsystem::QualityMinPoolPercent = 70;
float32 SceneRendererSubsystem::QualityMaxPoolPercent = 85;
float32 SceneRendererSubsystem::QualityMinScale = 0.3f;
uint32 SceneRendererSubsystem::StreamingStats = 0;
String SceneRendererSubsystem::DefaultViewMode = String("SceneLitViewMode");

namespace
{
/// Frames a freed mesh's ranges wait before reuse, so no frame in flight reads them anew.
constexpr uint32 RetireFrames = 3;
/// Bytes after each page's geometry, so the visibility raster may read a whole word past its last triangle.
constexpr uint64 PagePadding = 16;
/// Frames a page must go unused before it may be evicted: longer than a readback takes to come back.
constexpr uint32 EvictionFrames = 8;
/// A mesh table slot or page record that belongs to no mesh.
constexpr uint32 NoMesh = ~0u;

bool IsViewStage(const TypeInfo &type)
{
    return type.GetKind() == TypeKind::Struct && type.GetMetadata().Has("SceneViewStage");
}

void ParseNames(Optional<StringView> value, List<StringView> &names)
{
    if (!value.HasValue())
    {
        return;
    }
    StringView text = value.Value().Trim();
    if (text.StartsWith('(') && text.EndsWith(')'))
    {
        text = text.Substring(1, text.Length() - 2);
    }
    for (const StringView part : text.Split(','))
    {
        const StringView name = part.Trim();
        if (!name.IsEmpty())
        {
            names.Add(name);
        }
    }
}

const TypeInfo &ResolveStage(const TypeInfo &owner, StringView key, StringView name)
{
    const TypeInfo *type = Reflection::FindType(name);
    if (type == nullptr || !IsViewStage(*type))
    {
        MARLIN_FATALF("{0}: {1}={2} does not name a scene view stage", owner.GetName(), key, name);
    }
    return *type;
}

template <typename T> Span<const uint8> AsBytes(const T &value)
{
    return {reinterpret_cast<const uint8 *>(&value), sizeof(T)};
}

SceneGpuView MakeViewData(const SceneViewSnapshot &view)
{
    const SceneCamera &camera = view.Camera;
    const Vector3 right = camera.Rotation.Rotate(Vector3::Right);
    const Vector3 up = camera.Rotation.Rotate(Vector3::Up);
    const Vector3 forward = camera.Rotation.Rotate(Vector3::Forward);
    const float32 scaleY = 1.0f / Math::Tan(camera.VerticalFov * 0.5f);
    const float32 scaleX = scaleY * static_cast<float32>(view.Height) / static_cast<float32>(view.Width);
    SceneGpuView data;
    data.Right[0] = right.X;
    data.Right[1] = right.Y;
    data.Right[2] = right.Z;
    data.Right[3] = scaleX;
    data.Up[0] = up.X;
    data.Up[1] = up.Y;
    data.Up[2] = up.Z;
    data.Up[3] = scaleY;
    data.Forward[0] = forward.X;
    data.Forward[1] = forward.Y;
    data.Forward[2] = forward.Z;
    data.Forward[3] = camera.Near;
    const SceneTilePosition position = SceneTilePosition::FromWorld(camera.Position);
    for (uint32 axis = 0; axis < 3; ++axis)
    {
        data.Tile[axis] = position.Tile[axis];
        data.Offset[axis] = position.Offset[axis];
    }
    data.Width = view.Width;
    data.Height = view.Height;
    return data;
}
} // namespace

RenderGraphBuffer SceneUploads::Upload(RenderGraphBuilder &graph, StringView name, Span<const uint8> bytes)
{
    RhiBufferDesc desc;
    desc.Size = Max<uint64>((bytes.Count() + 15) & ~uint64{15}, 16);
    desc.DebugName = name;
    const RenderGraphBuffer buffer = graph.CreateBuffer(name, desc);
    const Region region{0, bytes};
    UploadTo(graph, name, buffer, Span<const Region>(&region, 1));
    return buffer;
}

void SceneUploads::UploadTo(RenderGraphBuilder &graph, StringView name, RenderGraphBuffer target,
                            Span<const Region> regions)
{
    struct Copy
    {
        uint64 TargetOffset = 0;
        RhiBuffer Source;
        uint64 SourceOffset = 0;
        uint64 Size = 0;
    };
    struct CopyData
    {
        RenderGraphView Target;
        List<Copy> Copies;
    };
    List<Copy> copies;
    for (const Region &region : regions)
    {
        if (region.Bytes.IsEmpty())
        {
            continue;
        }
        const RhiUploadAllocation allocation = _allocator->Allocate(region.Bytes.Count(), 16);
        if (allocation.Data == nullptr)
        {
            MARLIN_LOG(LogRenderer, Error, "{0}: out of upload memory for {1} bytes", name, region.Bytes.Count());
            continue;
        }
        MemCopy(allocation.Data, region.Bytes.Data(), region.Bytes.Count());
        copies.Add({region.Offset, allocation.Buffer, allocation.Offset, region.Bytes.Count()});
    }
    if (copies.IsEmpty())
    {
        return;
    }
    graph.AddPass<CopyData>(
        name, RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, CopyData &data) {
            data.Target = pass.Buffer(target, RhiAccess::CopyDest);
            data.Copies = Move(copies);
        },
        [target](const CopyData &data, RenderGraphContext &context) {
            const RhiBuffer buffer = context.GetBuffer(target);
            for (const Copy &copy : data.Copies)
            {
                context.List.CopyBuffer(buffer, copy.TargetOffset, copy.Source, copy.SourceOffset, copy.Size);
            }
        });
}

void SceneRangeAllocator::Reset(uint64 size)
{
    _free.Clear();
    if (size > 0)
    {
        _free.Add({0, size});
    }
}

bool SceneRangeAllocator::Allocate(uint64 size, uint64 alignment, uint64 &offset)
{
    for (usize index = 0; index < _free.Count(); ++index)
    {
        Range &range = _free[index];
        const uint64 start = (range.Offset + alignment - 1) / alignment * alignment;
        if (start + size > range.Offset + range.Size)
        {
            continue;
        }
        const uint64 end = start + size;
        const uint64 rangeEnd = range.Offset + range.Size;
        offset = start;
        if (start > range.Offset)
        {
            // Keep the gap before the aligned start; the part after it follows.
            const Range tail{end, rangeEnd - end};
            range.Size = start - range.Offset;
            if (tail.Size > 0)
            {
                _free.Insert(index + 1, tail);
            }
        }
        else if (end < rangeEnd)
        {
            range.Offset = end;
            range.Size = rangeEnd - end;
        }
        else
        {
            _free.RemoveAt(index);
        }
        return true;
    }
    return false;
}

void SceneRangeAllocator::Free(uint64 offset, uint64 size)
{
    if (size == 0)
    {
        return;
    }
    usize index = 0;
    while (index < _free.Count() && _free[index].Offset < offset)
    {
        ++index;
    }
    _free.Insert(index, Range{offset, size});
    // Merge with the next range, then with the previous one.
    if (index + 1 < _free.Count() && _free[index].Offset + _free[index].Size == _free[index + 1].Offset)
    {
        _free[index].Size += _free[index + 1].Size;
        _free.RemoveAt(index + 1);
    }
    if (index > 0 && _free[index - 1].Offset + _free[index - 1].Size == _free[index].Offset)
    {
        _free[index - 1].Size += _free[index].Size;
        _free.RemoveAt(index);
    }
}

struct SceneRendererSubsystem::MeshEntry
{
    enum class State : uint8
    {
        /// Waiting for the asset to load before its root pages are read.
        Waiting,
        /// Reading its root pages; it draws once they are resident.
        Reading,
        Resident,
        Failed,
        /// No references: retiring, then free.
        Released,
    };

    AssetRef<Mesh> Ref;
    Guid Uuid;
    uint32 References = 0;
    State Status = State::Waiting;
    BulkRequestId Request = 0;
    uint64 FirstNode = 0;
    uint64 FirstGroup = 0;
    uint64 FirstPage = 0;
    uint32 NodeCount = 0;
    uint32 GroupCount = 0;
    uint32 PageCount = 0;
    /// Its sections in the section table, and the material slot each holds a reference to.
    uint64 FirstSection = 0;
    List<uint32> MaterialSlots;
};

/// A mesh drawn with other materials for some slots: the section table its instances use instead of the mesh's.
struct SceneRendererSubsystem::MaterialBinding
{
    uint32 Mesh = SceneInvalidMesh;
    /// Per section, the material instead of its default, or a null Guid.
    List<Guid> Overrides;
    uint32 References = 0;
    /// Its sections are in the section table (or could not be): FirstSection and a material slot reference each.
    bool Made = false;
    uint64 FirstSection = 0;
    List<uint32> MaterialSlots;
};

/// A page of some mesh, by its index in the page table.
struct SceneRendererSubsystem::PageRecord
{
    enum class State : uint8
    {
        Absent,
        /// Asked for; its read has not started.
        Queued,
        Loading,
        /// Read, waiting for its parents to be resident.
        Loaded,
        Resident,
    };

    uint32 Mesh = NoMesh;
    uint32 Slot = SceneNotResident;
    uint32 SlotCount = 0;
    /// Resident pages that list this one as a parent: it may not go while any is there.
    uint32 ResidentChildren = 0;
    /// The last frame stamp a view used it in.
    uint32 LastUsed = 0;
    State Status = State::Absent;
    /// While queued: the largest error on screen, in pixels, that the latest readback asking for it gave, raised
    /// to its children's; the frame stamp of that readback; and how many generations of children it is queued
    /// for, so that a parent loads before a child of the same priority.
    float32 Priority = 0;
    uint32 PriorityStamp = 0;
    uint32 Generation = 0;
    /// A root page, resident as long as its mesh.
    bool Pinned = false;
    BulkRequestId Request = 0;
    SharedPtr<const BulkBytes> Bytes;
};

SceneRendererSubsystem::SceneRendererSubsystem() = default;
SceneRendererSubsystem::~SceneRendererSubsystem() = default;

void SceneRendererSubsystem::Initialize(SubsystemContext &context)
{
    RendererSubsystem *renderer = context.Collection->Get<RendererSubsystem>();
    _device = renderer != nullptr ? renderer->GetDevice() : nullptr;
    _shaders = context.Collection->Get<ShaderLibrary>();
    AssetSubsystem *assets = context.Collection->Get<AssetSubsystem>();
    _streamer = assets != nullptr ? &assets->GetStreamer() : nullptr;
    _nodeRanges.Reset(MaxNodes);
    _groupRanges.Reset(MaxGroups);
    _pageRanges.Reset(MaxPages);
    _sectionRanges.Reset(MaxSections);
    _slotRanges.Reset(GetSlotCount());
    _pages.Resize(MaxPages);
    _slotPages.Resize(GetSlotCount(), SceneNotResident);
    if (_device != nullptr)
    {
        _uploadAllocator = MakeUnique<RhiUploadAllocator>(*_device);
        _textures = MakeUnique<SceneTextureResidency>(*_device, _streamer);
        _materials = MakeUnique<SceneMaterialRegistry>(_textures.Get());
    }
    DiscoverStages();
    _features.Build();
    for (SceneViewFeature *feature : _features)
    {
        const TypeInfo &type = _features.GetType(*feature);
        const Optional<StringView> stageName = type.GetMetadata().Get("Stage");
        if (!stageName.HasValue())
        {
            MARLIN_FATALF("{0} names no Stage", type.GetName());
        }
        const TypeInfo &stage = ResolveStage(type, "Stage", stageName.Value().Trim());
        for (Stage &entry : _stages)
        {
            if (entry.Type == &stage)
            {
                entry.Features.Add(feature);
            }
        }
    }
    _modes.Build();
    MARLIN_LOG(LogRenderer, Debug, "Scene renderer: {0} view modes, {1} view stages", _modes.Count(), _stages.Count());
    for (SceneViewMode *mode : _modes)
    {
        if (_modes.GetType(*mode).GetName() == DefaultViewMode.AsView())
        {
            _defaultMode = mode;
        }
    }
    if (_defaultMode == nullptr && _modes.Count() > 0)
    {
        MARLIN_LOG(LogRenderer, Warning, "Engine.SceneRenderer.DefaultViewMode {0} is no SceneViewMode",
                   DefaultViewMode);
        _defaultMode = *_modes.begin();
    }
}

void SceneRendererSubsystem::Deinitialize()
{
    if (RendererSubsystem *renderer = Engine::GetSubsystem<RendererSubsystem>())
    {
        renderer->WaitForRender();
    }
    // Worlds may outlive the renderer: their scenes stop drawing with it now.
    for (RenderScene *scene : _scenes)
    {
        scene->Detach();
    }
    _scenes.Clear();
    if (_device != nullptr)
    {
        _device->WaitIdle();
        for (RenderGraphExternalBuffer *buffer : {&_geometry, &_clusters, &_meshTable, &_nodes, &_groups, &_pageTable,
                                                  &_pageRequested, &_slotUsed, &_requests})
        {
            if (buffer->Buffer.IsValid())
            {
                _device->Destroy(buffer->Buffer);
                *buffer = {};
            }
        }
        for (Readback &readback : _readbacks)
        {
            if (readback.Buffer.IsValid())
            {
                _device->Destroy(readback.Buffer);
                readback = {};
            }
        }
    }
    // The materials release their textures into the residency, and both let go of their assets while the asset
    // system still runs.
    _materials.Reset();
    _textures.Reset();
    {
        const ScopeLock lock(_meshLock);
        for (UniquePtr<MeshEntry> &entry : _meshes)
        {
            if (entry->Status == MeshEntry::State::Reading && _streamer != nullptr)
            {
                _streamer->Cancel(entry->Request);
            }
            ReleasePages(*entry);
        }
        _meshes.Clear();
        _pages.Clear();
        _slotPages.Clear();
        _residentPages.Clear();
        _loadQueue.Clear();
        _loadedPages.Clear();
        _meshByGuid.Clear();
        _uploads.Clear();
    }
    _renderUploads.Clear();
    _uploadAllocator.Reset();
    _stages.Clear();
    _scenes.Clear();
}

void SceneRendererSubsystem::DiscoverStages()
{
    List<const TypeInfo *> stages;
    Reflection::ForEachType([&](const TypeInfo &type) {
        if (IsViewStage(type))
        {
            stages.Add(&type);
        }
    });
    stages.Sort([](const TypeInfo *a, const TypeInfo *b) { return a->GetName().CompareTo(b->GetName()) < 0; });
    List<List<uint32>> successors;
    List<uint32> predecessors;
    successors.Resize(stages.Count());
    predecessors.Resize(stages.Count());
    const auto indexOf = [&](const TypeInfo &owner, StringView key, StringView name) {
        return static_cast<uint32>(stages.IndexOf(&ResolveStage(owner, key, name)));
    };
    for (uint32 index = 0; index < stages.Count(); ++index)
    {
        List<StringView> names;
        ParseNames(stages[index]->GetMetadata().Get("After"), names);
        for (const StringView name : names)
        {
            successors[indexOf(*stages[index], "After", name)].Add(index);
            ++predecessors[index];
        }
        names.Clear();
        ParseNames(stages[index]->GetMetadata().Get("Before"), names);
        for (const StringView name : names)
        {
            const uint32 next = indexOf(*stages[index], "Before", name);
            successors[index].Add(next);
            ++predecessors[next];
        }
    }
    List<uint32> ready;
    for (uint32 index = 0; index < stages.Count(); ++index)
    {
        if (predecessors[index] == 0)
        {
            ready.Add(index);
        }
    }
    while (!ready.IsEmpty())
    {
        const uint32 index = ready[0];
        ready.RemoveAt(0);
        _stages.AddDefaulted().Type = stages[index];
        for (const uint32 next : successors[index])
        {
            if (--predecessors[next] == 0)
            {
                ready.Add(next);
            }
        }
    }
    if (_stages.Count() != stages.Count())
    {
        MARLIN_FATAL("The scene view stages' After and Before form a cycle");
    }
}

void SceneRendererSubsystem::ViewModeCommand(const String &name)
{
    SceneRendererSubsystem *renderer = Engine::GetSubsystem<SceneRendererSubsystem>();
    if (MARLIN_BUILD_SHIPPING || renderer == nullptr)
    {
        return;
    }
    for (const SceneViewMode *mode : renderer->_modes)
    {
        const TypeInfo &type = renderer->_modes.GetType(*mode);
        if (!name.IsEmpty() &&
            (mode->GetDisplayName().EqualsIgnoreCase(name.AsView()) || type.GetName().EqualsIgnoreCase(name.AsView())))
        {
            renderer->_defaultMode = mode;
            DefaultViewMode = String(type.GetName());
            MARLIN_LOG(LogRenderer, Info, "View mode {0}", mode->GetDisplayName());
            return;
        }
    }
    StringBuilder<512> names;
    for (const SceneViewMode *mode : renderer->_modes)
    {
        FormatTo(names, "{0}{1}", names.Length() > 0 ? ", " : "", mode->GetDisplayName());
    }
    MARLIN_LOG(LogRenderer, Info, "View modes: {0}", names.AsView());
}

const SceneViewMode *SceneRendererSubsystem::FindViewMode(const TypeInfo *type) const
{
    if (type != nullptr)
    {
        for (const SceneViewMode *mode : _modes)
        {
            if (&_modes.GetType(*mode) == type)
            {
                return mode;
            }
        }
    }
    return _defaultMode;
}

void SceneRendererSubsystem::AddScene(RenderScene &scene)
{
    _scenes.Add(&scene);
}

void SceneRendererSubsystem::RemoveScene(RenderScene &scene)
{
    if (RendererSubsystem *renderer = Engine::GetSubsystem<RendererSubsystem>())
    {
        renderer->WaitForRender();
    }
    _scenes.Remove(&scene);
}

uint32 SceneRendererSubsystem::AcquireMesh(const AssetRef<Mesh> &mesh)
{
    const AssetSlot *slot = mesh.GetSlot();
    if (slot == nullptr)
    {
        return SceneInvalidMesh;
    }
    const ScopeLock lock(_meshLock);
    if (const uint32 *found = _meshByGuid.Find(slot->GetGuid()))
    {
        MeshEntry &entry = *_meshes[*found];
        if (entry.Status == MeshEntry::State::Released)
        {
            // Asked for again while retiring: it reads its pages anew.
            entry.Status = MeshEntry::State::Waiting;
            _pendingReads.Add(*found);
        }
        ++entry.References;
        return *found;
    }
    uint32 index = 0;
    if (!_freeMeshes.IsEmpty())
    {
        index = _freeMeshes.Pop();
    }
    else
    {
        if (_meshes.Count() >= MaxMeshes)
        {
            MARLIN_LOG(LogRenderer, Error, "The scene's mesh table is full ({0} meshes)", MaxMeshes);
            return SceneInvalidMesh;
        }
        index = static_cast<uint32>(_meshes.Count());
        _meshes.Add(MakeUnique<MeshEntry>());
    }
    MeshEntry &entry = *_meshes[index];
    entry = MeshEntry();
    entry.Ref = mesh;
    entry.Uuid = slot->GetGuid();
    entry.References = 1;
    _meshByGuid.FindOrAdd(entry.Uuid) = index;
    _pendingReads.Add(index);
    // The table entry draws nothing until the pages are resident.
    GeometryUpload &upload = _uploads.AddDefaulted();
    upload.Into = GeometryUpload::Target::Meshes;
    upload.Offset = uint64{index} * sizeof(SceneGpuMesh);
    upload.Bytes.AddRange(AsBytes(SceneGpuMesh()));
    return index;
}

void SceneRendererSubsystem::AddMeshReference(uint32 mesh)
{
    if (mesh == SceneInvalidMesh)
    {
        return;
    }
    const ScopeLock lock(_meshLock);
    ++_meshes[mesh]->References;
}

void SceneRendererSubsystem::ReleaseMesh(uint32 mesh)
{
    if (mesh == SceneInvalidMesh)
    {
        return;
    }
    const ScopeLock lock(_meshLock);
    MeshEntry &entry = *_meshes[mesh];
    MARLIN_ASSERTF(entry.References > 0, "Mesh {0} released more often than acquired", mesh);
    --entry.References;
}

uint32 SceneRendererSubsystem::AcquireMaterialBinding(uint32 mesh, Span<const SoftAssetRef<Material>> overrides)
{
    // Up to the last override, which names the binding.
    List<Guid> materials;
    for (const SoftAssetRef<Material> &material : overrides)
    {
        materials.Add(material.GetGuid());
    }
    while (!materials.IsEmpty() && !materials.Last().IsValid())
    {
        materials.Pop();
    }
    if (mesh == SceneInvalidMesh || materials.IsEmpty())
    {
        return 0;
    }
    const ScopeLock lock(_meshLock);
    for (uint32 id = 1; id < _bindings.Count(); ++id)
    {
        MaterialBinding &binding = *_bindings[id];
        bool same = binding.Mesh == mesh && binding.References != 0 && binding.Overrides.Count() == materials.Count();
        for (usize o = 0; same && o < materials.Count(); ++o)
        {
            same = binding.Overrides[o] == materials[o];
        }
        if (same)
        {
            ++binding.References;
            return id;
        }
    }
    if (_bindings.IsEmpty())
    {
        // Id 0 is no binding.
        _bindings.Add(MakeUnique<MaterialBinding>());
        _bindingSections.Add(~0u);
    }
    uint32 id = 0;
    if (!_freeBindings.IsEmpty())
    {
        id = _freeBindings.Pop();
    }
    else
    {
        id = static_cast<uint32>(_bindings.Count());
        _bindings.Add(MakeUnique<MaterialBinding>());
        _bindingSections.Add(~0u);
    }
    MaterialBinding &binding = *_bindings[id];
    binding = MaterialBinding();
    binding.Mesh = mesh;
    binding.Overrides = Move(materials);
    binding.References = 1;
    // It holds its mesh, whose slot names make its table.
    ++_meshes[mesh]->References;
    _bindingSections[id] = ~0u;
    return id;
}

void SceneRendererSubsystem::AddMaterialBindingReference(uint32 binding)
{
    if (binding == 0)
    {
        return;
    }
    const ScopeLock lock(_meshLock);
    ++_bindings[binding]->References;
}

void SceneRendererSubsystem::ReleaseMaterialBinding(uint32 binding)
{
    if (binding == 0)
    {
        return;
    }
    const ScopeLock lock(_meshLock);
    MaterialBinding &entry = *_bindings[binding];
    MARLIN_ASSERTF(entry.References > 0, "Material binding {0} released more often than acquired", binding);
    --entry.References;
}

void SceneRendererSubsystem::UpdateMaterialBindings()
{
    for (uint32 id = 1; id < _bindings.Count(); ++id)
    {
        MaterialBinding &binding = *_bindings[id];
        if (binding.Mesh == SceneInvalidMesh)
        {
            continue;
        }
        if (binding.References == 0)
        {
            // Its sections go once the GPU is done with the frames that read them.
            for (const uint32 material : binding.MaterialSlots)
            {
                if (_materials != nullptr)
                {
                    _materials->Release(material);
                }
            }
            if (!binding.MaterialSlots.IsEmpty())
            {
                Retired &retired = _retired.AddDefaulted();
                retired.FirstSection = binding.FirstSection;
                retired.SectionCount = binding.MaterialSlots.Count();
                retired.Mesh = SceneInvalidMesh;
                retired.FramesLeft = RetireFrames;
            }
            --_meshes[binding.Mesh]->References;
            binding = MaterialBinding();
            _bindingSections[id] = ~0u;
            _freeBindings.Add(id);
            continue;
        }
        const MeshEntry &mesh = *_meshes[binding.Mesh];
        const Mesh *source = mesh.Ref.Get();
        if (binding.Made || source == nullptr || mesh.MaterialSlots.IsEmpty())
        {
            continue;
        }
        binding.Made = true;
        const usize sectionCount = mesh.MaterialSlots.Count();
        uint64 firstSection = 0;
        if (!_sectionRanges.Allocate(sectionCount, 1, firstSection))
        {
            MARLIN_LOG(LogRenderer, Error, "The section table has no room for a material list of mesh {0}", mesh.Uuid);
            continue;
        }
        for (usize s = 0; s < sectionCount; ++s)
        {
            const bool replaced = s < binding.Overrides.Count() && binding.Overrides[s].IsValid();
            const Guid material = replaced                       ? binding.Overrides[s]
                                  : s < source->Sections.Count() ? source->Sections[s].DefaultMaterial.GetGuid()
                                                                 : Guid();
            binding.MaterialSlots.Add(_materials != nullptr ? _materials->Acquire(material) : 0u);
        }
        if (binding.Overrides.Count() > source->Sections.Count())
        {
            MARLIN_LOG(LogRenderer, Warning, "Mesh {0} has {1} sections; the materials past them are ignored",
                       mesh.Uuid, source->Sections.Count());
        }
        binding.FirstSection = firstSection;
        AddUpload(GeometryUpload::Target::Sections, firstSection * sizeof(uint32),
                  Span<const uint8>(reinterpret_cast<const uint8 *>(binding.MaterialSlots.Data()),
                                    binding.MaterialSlots.Count() * sizeof(uint32)));
        _bindingSections[id] = static_cast<uint32>(firstSection);
    }
}

uint32 SceneRendererSubsystem::GetSlotCount() const
{
    const uint64 slotSize = Max(SlotSize, 4096u);
    const uint64 pool = Min<uint64>(StreamingPoolSize + uint64{RootPages} * slotSize, uint64{0xFFFFFFFF} + 1);
    return static_cast<uint32>(pool / slotSize);
}

void SceneRendererSubsystem::AddUpload(GeometryUpload::Target into, uint64 offset, Span<const uint8> bytes)
{
    GeometryUpload &upload = _uploads.AddDefaulted();
    upload.Into = into;
    upload.Offset = offset;
    upload.Bytes.AddRange(bytes);
}

void SceneRendererSubsystem::UploadMeshTable(uint32 mesh, uint32 depthCount)
{
    const MeshEntry &entry = *_meshes[mesh];
    SceneGpuMesh table;
    table.FirstNode = static_cast<uint32>(entry.FirstNode);
    table.DepthCount = depthCount;
    table.FirstGroup = static_cast<uint32>(entry.FirstGroup);
    table.FirstPage = static_cast<uint32>(entry.FirstPage);
    if (const Mesh *source = entry.Ref.Get())
    {
        const Vector3 center = (source->Bounds.Min + source->Bounds.Max) * 0.5f;
        table.Sphere[0] = center.X;
        table.Sphere[1] = center.Y;
        table.Sphere[2] = center.Z;
        table.Sphere[3] = (source->Bounds.Max - source->Bounds.Min).Length() * 0.5f;
        table.AttributeStride = source->AttributeStride;
        table.AttributeLayout = Min(source->TexCoordCount, 15u) | (source->HasTangents ? SceneAttributeTangents : 0u) |
                                (source->HasColors ? SceneAttributeColors : 0u);
    }
    table.FirstSection = static_cast<uint32>(entry.FirstSection);
    AddUpload(GeometryUpload::Target::Meshes, uint64{mesh} * sizeof(SceneGpuMesh), AsBytes(table));
}

void SceneRendererSubsystem::BeginFrame(const FrameTime & /*time*/)
{
    if (_textures != nullptr)
    {
        SceneTextureSettings settings;
        settings.Streaming = TextureStreaming;
        settings.PoolSize = uint64{TexturePoolSize} << 20;
        settings.MipBias = TextureMipBias;
        settings.MaxAnisotropy = MaxAnisotropy;
        _textures->Update(settings);
    }
    if (_materials != nullptr)
    {
        _materials->Update();
    }
    const ScopeLock lock(_meshLock);
    // Bindings first: one nothing uses lets go of its mesh, which then retires with the others.
    UpdateMaterialBindings();
    // Meshes nothing references any more retire: their pages go now, their table ranges after a few frames.
    for (uint32 index = 0; index < _meshes.Count(); ++index)
    {
        MeshEntry &entry = *_meshes[index];
        if (entry.References != 0 || entry.Status == MeshEntry::State::Released || !entry.Ref.GetSlot())
        {
            continue;
        }
        if (entry.Status == MeshEntry::State::Reading && _streamer != nullptr)
        {
            _streamer->Cancel(entry.Request);
        }
        ReleasePages(entry);
        AddUpload(GeometryUpload::Target::Meshes, uint64{index} * sizeof(SceneGpuMesh), AsBytes(SceneGpuMesh()));
        Retired &retired = _retired.AddDefaulted();
        retired.FirstNode = entry.FirstNode;
        retired.NodeCount = entry.NodeCount;
        retired.FirstGroup = entry.FirstGroup;
        retired.GroupCount = entry.GroupCount;
        retired.FirstPage = entry.FirstPage;
        retired.PageCount = entry.PageCount;
        retired.FirstSection = entry.FirstSection;
        retired.SectionCount = entry.MaterialSlots.Count();
        for (const uint32 material : entry.MaterialSlots)
        {
            if (_materials != nullptr)
            {
                _materials->Release(material);
            }
        }
        entry.MaterialSlots.Clear();
        retired.Mesh = index;
        retired.FramesLeft = RetireFrames;
        entry.NodeCount = 0;
        entry.GroupCount = 0;
        entry.PageCount = 0;
        entry.Status = MeshEntry::State::Released;
    }
    for (usize index = 0; index < _retired.Count();)
    {
        Retired &retired = _retired[index];
        if (--retired.FramesLeft > 0)
        {
            ++index;
            continue;
        }
        _nodeRanges.Free(retired.FirstNode, retired.NodeCount);
        _groupRanges.Free(retired.FirstGroup, retired.GroupCount);
        _pageRanges.Free(retired.FirstPage, retired.PageCount);
        _sectionRanges.Free(retired.FirstSection, retired.SectionCount);
        // A material binding's sections retire alone.
        if (retired.Mesh != SceneInvalidMesh)
        {
            MeshEntry &entry = *_meshes[retired.Mesh];
            if (entry.Status == MeshEntry::State::Released)
            {
                _meshByGuid.Remove(entry.Uuid);
                entry = MeshEntry();
                _freeMeshes.Add(retired.Mesh);
            }
        }
        _retired.RemoveAtSwap(index);
    }
    StartReads();
}

void SceneRendererSubsystem::ReleasePages(MeshEntry &entry)
{
    // With the lock held. The pages of one mesh only depend on each other, so no other page's counts change.
    for (uint64 page = entry.FirstPage; page < entry.FirstPage + entry.PageCount; ++page)
    {
        PageRecord &record = _pages[page];
        if (record.Status == PageRecord::State::Loading)
        {
            if (_streamer != nullptr)
            {
                _streamer->Cancel(record.Request);
            }
            --_loadsInFlight;
        }
        if (record.Status == PageRecord::State::Resident)
        {
            _slotRanges.Free(record.Slot, record.SlotCount);
            _slotPages[record.Slot] = SceneNotResident;
            _residentPages.Remove(static_cast<uint32>(page));
        }
        record = PageRecord();
    }
}

void SceneRendererSubsystem::StartReads()
{
    // With the lock held, on the main thread.
    for (usize position = 0; position < _pendingReads.Count();)
    {
        const uint32 index = _pendingReads[position];
        MeshEntry &entry = *_meshes[index];
        if (entry.Status != MeshEntry::State::Waiting)
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
        const Mesh *mesh = entry.Ref.Get();
        if (state == AssetState::Failed || mesh == nullptr || mesh->Pages.IsEmpty() || mesh->RootPageCount == 0 ||
            _streamer == nullptr)
        {
            entry.Status = MeshEntry::State::Failed;
            continue;
        }
        // Its hierarchy, groups and page table entries stay resident while it is used.
        uint64 firstNode = 0;
        uint64 firstGroup = 0;
        uint64 firstPage = 0;
        const bool nodes = _nodeRanges.Allocate(mesh->Hierarchy.Count(), 1, firstNode);
        const bool groups = nodes && _groupRanges.Allocate(mesh->Groups.Count(), 1, firstGroup);
        const bool pages = groups && _pageRanges.Allocate(mesh->Pages.Count(), 1, firstPage);
        uint64 firstSection = 0;
        const usize sectionCount = Max<usize>(mesh->Sections.Count(), 1);
        const bool sections = pages && _sectionRanges.Allocate(sectionCount, 1, firstSection);
        if (pages && !sections)
        {
            _pageRanges.Free(firstPage, mesh->Pages.Count());
        }
        if (!sections)
        {
            MARLIN_LOG(LogRenderer, Error,
                       "The geometry tables have no room for mesh {0} ({1} nodes, {2} groups, {3} pages)", entry.Uuid,
                       mesh->Hierarchy.Count(), mesh->Groups.Count(), mesh->Pages.Count());
            if (nodes)
            {
                _nodeRanges.Free(firstNode, mesh->Hierarchy.Count());
            }
            if (groups)
            {
                _groupRanges.Free(firstGroup, mesh->Groups.Count());
            }
            entry.Status = MeshEntry::State::Failed;
            continue;
        }
        entry.FirstNode = firstNode;
        entry.NodeCount = static_cast<uint32>(mesh->Hierarchy.Count());
        entry.FirstGroup = firstGroup;
        entry.GroupCount = static_cast<uint32>(mesh->Groups.Count());
        entry.FirstPage = firstPage;
        entry.PageCount = static_cast<uint32>(mesh->Pages.Count());
        // Each section's material slot: its default material, or the engine's Default material.
        entry.FirstSection = firstSection;
        entry.MaterialSlots.Clear();
        for (usize s = 0; s < sectionCount; ++s)
        {
            const Guid material = s < mesh->Sections.Count() ? mesh->Sections[s].DefaultMaterial.GetGuid() : Guid();
            entry.MaterialSlots.Add(_materials != nullptr ? _materials->Acquire(material) : 0u);
        }
        for (uint64 page = firstPage; page < firstPage + entry.PageCount; ++page)
        {
            _pages[page] = PageRecord();
            _pages[page].Mesh = index;
        }
        List<SceneGpuNode> nodeTable;
        nodeTable.Resize(mesh->Hierarchy.Count());
        for (usize n = 0; n < mesh->Hierarchy.Count(); ++n)
        {
            const MeshHierarchyNode &source = mesh->Hierarchy[n];
            SceneGpuNode &node = nodeTable[n];
            node.Sphere[0] = source.Bounds.Center.X;
            node.Sphere[1] = source.Bounds.Center.Y;
            node.Sphere[2] = source.Bounds.Center.Z;
            node.Sphere[3] = source.Bounds.Radius;
            node.Error = source.Bounds.Error;
            node.Group = source.Group;
            node.FirstChild = source.FirstChild;
            node.ChildCount = source.ChildCount;
        }
        List<SceneGpuGroup> groupTable;
        groupTable.Resize(mesh->Groups.Count());
        for (usize g = 0; g < mesh->Groups.Count(); ++g)
        {
            const MeshClusterGroup &source = mesh->Groups[g];
            SceneGpuGroup &group = groupTable[g];
            group.Sphere[0] = source.Simplified.Center.X;
            group.Sphere[1] = source.Simplified.Center.Y;
            group.Sphere[2] = source.Simplified.Center.Z;
            group.Sphere[3] = source.Simplified.Radius;
            group.Error = source.Simplified.Error;
            group.Page = source.Page;
            group.ClusterRange = source.FirstCluster | (source.ClusterCount << 16);
            group.Depth = source.Depth;
        }
        List<uint32> pageTable;
        pageTable.Resize(mesh->Pages.Count(), SceneNotResident);
        const auto bytesOf = [](const auto &list) {
            return Span<const uint8>(reinterpret_cast<const uint8 *>(list.Data()), list.Count() * sizeof(list[0]));
        };
        AddUpload(GeometryUpload::Target::Nodes, firstNode * sizeof(SceneGpuNode), bytesOf(nodeTable));
        AddUpload(GeometryUpload::Target::Groups, firstGroup * sizeof(SceneGpuGroup), bytesOf(groupTable));
        AddUpload(GeometryUpload::Target::PageTable, firstPage * sizeof(uint32), bytesOf(pageTable));
        AddUpload(GeometryUpload::Target::Sections, firstSection * sizeof(uint32), bytesOf(entry.MaterialSlots));
        UploadMeshTable(index, 0);

        // The root pages, the coarsest levels, in one read: they come first in the bulk data.
        const MeshPage &first = mesh->Pages[0];
        const MeshPage &last = mesh->Pages[mesh->RootPageCount - 1];
        entry.Status = MeshEntry::State::Reading;
        entry.Request = _streamer->Request(
            *slot, first.Offset, last.Offset + last.Size - first.Offset, 1.0f,
            [this, index](BulkRequestId /*request*/, SharedPtr<const BulkBytes> bytes) {
                OnRootPagesRead(index, Move(bytes));
            },
            IoPriority::Streaming);
    }
}

void SceneRendererSubsystem::OnRootPagesRead(uint32 index, SharedPtr<const BulkBytes> bytes)
{
    const ScopeLock lock(_meshLock);
    MeshEntry &entry = *_meshes[index];
    if (entry.Status != MeshEntry::State::Reading)
    {
        return;
    }
    const Mesh *mesh = entry.Ref.Get();
    if (bytes == nullptr || mesh == nullptr)
    {
        MARLIN_LOG(LogRenderer, Error, "The root pages of mesh {0} could not be read", entry.Uuid);
        entry.Status = MeshEntry::State::Failed;
        return;
    }
    const uint64 base = mesh->Pages[0].Offset;
    for (uint32 root = 0; root < mesh->RootPageCount; ++root)
    {
        const MeshPage &page = mesh->Pages[root];
        if (!CommitPage(static_cast<uint32>(entry.FirstPage + root),
                        bytes->GetBytes().Slice(static_cast<usize>(page.Offset - base), page.Size), true))
        {
            MARLIN_LOG(LogRenderer, Error, "The streaming pool has no room for the root pages of mesh {0}", entry.Uuid);
            ReleasePages(entry);
            entry.Status = MeshEntry::State::Failed;
            return;
        }
    }
    UploadMeshTable(index, mesh->DepthCount);
    entry.Status = MeshEntry::State::Resident;
}

const MeshPage *SceneRendererSubsystem::FindPage(uint32 page, MeshEntry *&entry) const
{
    entry = nullptr;
    if (page >= _pages.Count() || _pages[page].Mesh == NoMesh)
    {
        return nullptr;
    }
    entry = _meshes[_pages[page].Mesh].Get();
    const Mesh *mesh = entry->Ref.Get();
    if (mesh == nullptr || page < entry->FirstPage || page >= entry->FirstPage + entry->PageCount)
    {
        return nullptr;
    }
    return &mesh->Pages[page - entry->FirstPage];
}

bool SceneRendererSubsystem::CommitPage(uint32 page, Span<const uint8> bytes, bool pinned)
{
    MeshEntry *entry = nullptr;
    const MeshPage *source = FindPage(page, entry);
    if (source == nullptr || bytes.Count() < source->Size)
    {
        return false;
    }
    const Mesh &mesh = *entry->Ref.Get();
    const uint32 geometryOffset = source->GetGeometryOffset();
    const uint64 geometryBytes = source->Size - geometryOffset;
    const uint32 slotSize = Max(SlotSize, 4096u);
    const auto slots = static_cast<uint32>(Max<uint64>((geometryBytes + PagePadding + slotSize - 1) / slotSize, 1));
    uint64 first = 0;
    if (!_slotRanges.Allocate(slots, 1, first) && (!FreeSlots(slots) || !_slotRanges.Allocate(slots, 1, first)))
    {
        return false;
    }
    const auto slot = static_cast<uint32>(first);
    const uint64 geometryBase = uint64{slot} * slotSize;

    // The cluster records, with their offsets made absolute in the geometry pool.
    List<SceneGpuCluster> clusters;
    clusters.Resize(source->ClusterCount);
    for (uint32 c = 0; c < source->ClusterCount; ++c)
    {
        MeshPageCluster record;
        MemCopy(&record, bytes.Data() + usize{c} * sizeof(MeshPageCluster), sizeof(record));
        SceneGpuCluster &cluster = clusters[c];
        cluster.VertexOffset = static_cast<uint32>(geometryBase + record.VertexOffset);
        cluster.IndexOffset = static_cast<uint32>(geometryBase + record.IndexOffset);
        cluster.Counts = record.VertexCount | (record.TriangleCount << 9);
        cluster.Section = record.Section;
        cluster.PositionStep = mesh.PositionStep;
        cluster.RefinedGroup = record.RefinedGroup;
        cluster.Group = record.Group;
        cluster.MaxEdgeLength = record.MaxEdgeLength;
        MemCopy(cluster.Sphere, record.Sphere, sizeof(cluster.Sphere));
        MemCopy(cluster.Cone, record.Cone, sizeof(cluster.Cone));
        MemCopy(cluster.PositionMin, record.PositionMin, sizeof(cluster.PositionMin));
        cluster.PositionBits = record.PositionBits;
    }
    AddUpload(GeometryUpload::Target::Clusters, uint64{slot} * SceneSlotClusters * sizeof(SceneGpuCluster),
              Span<const uint8>(reinterpret_cast<const uint8 *>(clusters.Data()),
                                clusters.Count() * sizeof(SceneGpuCluster)));
    AddUpload(GeometryUpload::Target::Geometry, geometryBase,
              bytes.Slice(geometryOffset, static_cast<usize>(geometryBytes)));
    AddUpload(GeometryUpload::Target::PageTable, uint64{page} * sizeof(uint32), AsBytes(slot));

    PageRecord &record = _pages[page];
    record.Slot = slot;
    record.SlotCount = slots;
    record.Status = PageRecord::State::Resident;
    record.Pinned = pinned;
    record.LastUsed = _frameStamp;
    record.Bytes = nullptr;
    _slotPages[slot] = page;
    _residentPages.Add(page);
    ++_statCommits;
    for (uint32 p = 0; p < source->ParentCount; ++p)
    {
        ++_pages[entry->FirstPage + mesh.PageParents[source->FirstParent + p]].ResidentChildren;
    }
    return true;
}

bool SceneRendererSubsystem::FreeSlots(uint32 slots)
{
    // The pages unused longest go first; only those no resident page depends on, and never a root page.
    List<uint32> candidates;
    for (const uint32 page : _residentPages)
    {
        const PageRecord &record = _pages[page];
        if (!record.Pinned && record.ResidentChildren == 0 && record.LastUsed + EvictionFrames <= _frameStamp)
        {
            candidates.Add(page);
        }
    }
    candidates.Sort([&](uint32 a, uint32 b) { return _pages[a].LastUsed < _pages[b].LastUsed; });
    for (const uint32 page : candidates)
    {
        EvictPage(page);
        uint64 offset = 0;
        if (_slotRanges.Allocate(slots, 1, offset))
        {
            _slotRanges.Free(offset, slots);
            return true;
        }
    }
    return false;
}

void SceneRendererSubsystem::EvictPage(uint32 page)
{
    PageRecord &record = _pages[page];
    MeshEntry *entry = nullptr;
    const MeshPage *source = FindPage(page, entry);
    AddUpload(GeometryUpload::Target::PageTable, uint64{page} * sizeof(uint32), AsBytes(SceneNotResident));
    _slotRanges.Free(record.Slot, record.SlotCount);
    _slotPages[record.Slot] = SceneNotResident;
    _residentPages.Remove(page);
    if (source != nullptr)
    {
        const Mesh &mesh = *entry->Ref.Get();
        for (uint32 p = 0; p < source->ParentCount; ++p)
        {
            --_pages[entry->FirstPage + mesh.PageParents[source->FirstParent + p]].ResidentChildren;
        }
    }
    record.Slot = SceneNotResident;
    record.SlotCount = 0;
    record.Status = PageRecord::State::Absent;
    ++_statEvictions;
}

void SceneRendererSubsystem::RequestPage(uint32 page, float32 priority, uint32 depth)
{
    MeshEntry *entry = nullptr;
    const MeshPage *source = FindPage(page, entry);
    PageRecord *record = source != nullptr ? &_pages[page] : nullptr;
    if (record == nullptr || depth > 64 ||
        (record->Status != PageRecord::State::Absent && record->Status != PageRecord::State::Queued))
    {
        return;
    }
    // A request of an earlier readback is replaced: what a view needs now counts, not what it needed before.
    if (record->Status == PageRecord::State::Absent || record->PriorityStamp != _frameStamp)
    {
        record->Priority = priority;
        record->Generation = depth;
    }
    else if (priority > record->Priority || depth > record->Generation)
    {
        record->Priority = Max(record->Priority, priority);
        record->Generation = Max(record->Generation, depth);
    }
    else
    {
        // Nothing to raise, here or in the parents: stopping keeps the walk from taking every path of the DAG.
        return;
    }
    record->PriorityStamp = _frameStamp;
    if (record->Status == PageRecord::State::Absent)
    {
        record->Status = PageRecord::State::Queued;
        _loadQueue.Add(page);
    }
    // Parents first: a page draws only once they are resident, so they load at least as early.
    const Mesh &mesh = *entry->Ref.Get();
    for (uint32 p = 0; p < source->ParentCount; ++p)
    {
        RequestPage(static_cast<uint32>(entry->FirstPage + mesh.PageParents[source->FirstParent + p]), priority,
                    depth + 1);
    }
}

void SceneRendererSubsystem::StartPageLoads()
{
    if (_loadsInFlight >= MaxPageLoads || _loadQueue.IsEmpty() || _streamer == nullptr)
    {
        return;
    }
    // The pages that sharpen the picture most first; of equal ones, parents before their children.
    usize kept = 0;
    for (const uint32 page : _loadQueue)
    {
        if (_pages[page].Status == PageRecord::State::Queued)
        {
            _loadQueue[kept++] = page;
        }
    }
    _loadQueue.Resize(kept);
    _loadQueue.Sort([&](uint32 a, uint32 b) {
        const PageRecord &first = _pages[a];
        const PageRecord &second = _pages[b];
        return first.Priority != second.Priority ? first.Priority > second.Priority
                                                 : first.Generation > second.Generation;
    });
    usize taken = 0;
    while (_loadsInFlight < MaxPageLoads && taken < _loadQueue.Count())
    {
        const uint32 page = _loadQueue[taken++];
        PageRecord &record = _pages[page];
        MeshEntry *entry = nullptr;
        const MeshPage *source = FindPage(page, entry);
        const AssetSlot *slot = entry != nullptr ? entry->Ref.GetSlot() : nullptr;
        if (source == nullptr || slot == nullptr)
        {
            record.Status = PageRecord::State::Absent;
            continue;
        }
        record.Status = PageRecord::State::Loading;
        record.Request = _streamer->Request(
            *slot, source->Offset, source->Size, 1.0f,
            [this, page](BulkRequestId /*request*/, SharedPtr<const BulkBytes> bytes) {
                OnPageRead(page, Move(bytes));
            },
            IoPriority::Streaming);
        ++_loadsInFlight;
    }
    _loadQueue.RemoveAt(0, taken);
}

void SceneRendererSubsystem::OnPageRead(uint32 page, SharedPtr<const BulkBytes> bytes)
{
    const ScopeLock lock(_meshLock);
    if (page >= _pages.Count() || _pages[page].Status != PageRecord::State::Loading)
    {
        return;
    }
    --_loadsInFlight;
    PageRecord &record = _pages[page];
    if (bytes == nullptr)
    {
        // Asked for again by a later frame if still needed.
        record.Status = PageRecord::State::Absent;
        return;
    }
    record.Bytes = Move(bytes);
    record.Status = PageRecord::State::Loaded;
    _loadedPages.Add(page);
}

void SceneRendererSubsystem::CommitLoadedPages()
{
    uint32 committed = 0;
    for (usize index = 0; index < _loadedPages.Count() && committed < MaxPageCommits;)
    {
        const uint32 page = _loadedPages[index];
        PageRecord &record = _pages[page];
        MeshEntry *entry = nullptr;
        const MeshPage *source = FindPage(page, entry);
        if (record.Status != PageRecord::State::Loaded || source == nullptr)
        {
            _loadedPages.RemoveAt(index);
            continue;
        }
        const Mesh &mesh = *entry->Ref.Get();
        bool parentsResident = true;
        for (uint32 p = 0; p < source->ParentCount; ++p)
        {
            const auto parent = static_cast<uint32>(entry->FirstPage + mesh.PageParents[source->FirstParent + p]);
            if (_pages[parent].Status != PageRecord::State::Resident)
            {
                parentsResident = false;
                RequestPage(parent, record.Priority, record.Generation + 1);
            }
        }
        if (!parentsResident)
        {
            ++index;
            continue;
        }
        if (!CommitPage(page, record.Bytes->GetBytes(), false))
        {
            // No room now: dropped, and asked for again while a view needs it.
            record.Status = PageRecord::State::Absent;
            record.Bytes = nullptr;
        }
        ++committed;
        _loadedPages.RemoveAt(index);
    }
}

void SceneRendererSubsystem::ProcessReadbacks()
{
    if (_device == nullptr)
    {
        return;
    }
    // The render job of the last frame has finished: its submission covers the readback it wrote.
    if (_lastReadback != ~0u && _readbacks[_lastReadback].Written)
    {
        Readback &readback = _readbacks[_lastReadback];
        readback.SubmitValue = _device->GetSubmittedValues().Values[static_cast<uint32>(RhiQueueType::Graphics)];
        readback.InFlight = true;
        readback.Written = false;
    }
    const uint64 completed = _device->GetCompletedValues().Values[static_cast<uint32>(RhiQueueType::Graphics)];
    const uint32 slots = GetSlotCount();
    for (Readback &readback : _readbacks)
    {
        if (!readback.InFlight || readback.SubmitValue > completed)
        {
            continue;
        }
        readback.InFlight = false;
        const auto *words = reinterpret_cast<const uint32 *>(_device->GetMappedData(readback.Buffer));
        if (words == nullptr)
        {
            continue;
        }
        const uint32 *used = words + SceneRequestHeaderWords + MaxPageRequests * SceneRequestWords;
        // The streamed pages the frame drew from, and those it asked for: what it needed of the pool.
        uint32 neededPages = words[0];
        for (uint32 slot = 0; slot < slots; ++slot)
        {
            const uint32 page = _slotPages[slot];
            if (page == SceneNotResident)
            {
                continue;
            }
            if (used[slot] > _pages[page].LastUsed)
            {
                _pages[page].LastUsed = used[slot];
            }
            neededPages += used[slot] == readback.Frame && !_pages[page].Pinned ? 1 : 0;
        }
        UpdateQualityScale(neededPages);
        const uint32 requests = Min(words[0], MaxPageRequests);
        _statRequests += words[0];
        _statQueuePeak = Max(_statQueuePeak, words[2]);
        _statTraversalFlags |= words[3];
        for (uint32 r = 0; r < requests; ++r)
        {
            const uint32 *request = words + SceneRequestHeaderWords + r * SceneRequestWords;
            RequestPage(request[0], BitCast<float32>(request[1]));
        }
    }
    // The readback this frame's render job copies into: one not waiting for the GPU.
    _renderReadback = ~0u;
    for (uint32 index = 0; index < 3; ++index)
    {
        if (!_readbacks[index].InFlight)
        {
            _renderReadback = index;
            break;
        }
    }
    _lastReadback = _renderReadback;
}

void SceneRendererSubsystem::UpdateQualityScale(uint32 neededPages)
{
    const float32 streamedSlots = static_cast<float32>(StreamingPoolSize / Max(SlotSize, 4096u));
    _lastPoolPercent = streamedSlots > 0 ? 100.0f * static_cast<float32>(neededPages) / streamedSlots : 100.0f;
    _framesOverfull = _lastPoolPercent > QualityMaxPoolPercent ? _framesOverfull + 1 : 0;
    _framesUnderfull = _lastPoolPercent < QualityMinPoolPercent ? _framesUnderfull + 1 : 0;
    if (_framesOverfull >= 2)
    {
        _qualityScale = Max(_qualityScale * 0.97f, QualityMinScale);
    }
    else if (_framesUnderfull >= 30)
    {
        _qualityScale = Min(_qualityScale * 1.01f, 1.0f);
    }
}

void SceneRendererSubsystem::Extract()
{
    if (_materials != nullptr)
    {
        _materials->Extract(_renderMaterials);
    }
    {
        const ScopeLock lock(_meshLock);
        ProcessReadbacks();
        CommitLoadedPages();
        StartPageLoads();
        _renderBindingSections = _bindingSections;
        // Stamps are never zero, which the persistent buffers start at.
        _frameStamp = _frameStamp == ~0u ? 1 : _frameStamp + 1;
        _renderFrameStamp = _frameStamp;
        _renderLodThreshold = LodPixelError / Max(_qualityScale, 0.01f);
        if (StreamingStats > 0 && _frameStamp % StreamingStats == 0)
        {
            uint32 usedSlots = 0;
            for (const uint32 page : _residentPages)
            {
                usedSlots += _pages[page].SlotCount;
            }
            MARLIN_LOG(LogRenderer, Info,
                       "Streaming: {0} pages in {1} of {2} slots, {3} loading, {4} queued, {5} waiting for parents; "
                       "{6} requested, {7} committed, {8} evicted since the last line; traversal queue peak "
                       "{9}{10}{11}{12}; pool {13}% needed, quality {14}",
                       _residentPages.Count(), usedSlots, GetSlotCount(), _loadsInFlight, _loadQueue.Count(),
                       _loadedPages.Count(), _statRequests, _statCommits, _statEvictions, _statQueuePeak,
                       (_statTraversalFlags & SceneTraversalQueueFull) != 0 ? ", queue full" : "",
                       (_statTraversalFlags & SceneTraversalGroupsFull) != 0 ? ", group list full" : "",
                       (_statTraversalFlags & SceneTraversalUnfinished) != 0 ? ", UNFINISHED" : "",
                       static_cast<uint32>(_lastPoolPercent), _qualityScale);
            _statRequests = 0;
            _statQueuePeak = 0;
            _statTraversalFlags = 0;
            _statCommits = 0;
            _statEvictions = 0;
        }
        for (GeometryUpload &upload : _uploads)
        {
            _renderUploads.Add(Move(upload));
        }
        _uploads.Clear();
    }
    if (_textures != nullptr)
    {
        _textures->Extract();
    }
    for (RenderScene *scene : _scenes)
    {
        scene->Extract(*this);
    }
}

bool SceneRendererSubsystem::EnsureGeometryBuffers()
{
    if (_geometry.Buffer.IsValid())
    {
        return true;
    }
    if (_device == nullptr)
    {
        return false;
    }
    const auto create = [&](uint64 size, StringView name, RenderGraphExternalBuffer &target, RhiBufferUsage usage) {
        RhiBufferDesc desc;
        desc.Size = Max<uint64>(size, 16);
        desc.Usage = usage;
        desc.DebugName = name;
        Result<RhiBuffer, RhiError> buffer = _device->CreateBuffer(desc);
        if (buffer.IsError())
        {
            MARLIN_LOG(LogRenderer, Error, "{0} of {1} bytes could not be created: {2}", name, size,
                       GetErrorName(buffer.Error()));
            return false;
        }
        target = {buffer.Value(), false};
        return true;
    };
    const RhiBufferUsage read = RhiBufferUsage::ShaderResource | RhiBufferUsage::CopyDest;
    const RhiBufferUsage written = read | RhiBufferUsage::UnorderedAccess | RhiBufferUsage::CopySource;
    const uint32 slots = GetSlotCount();
    const uint64 requestBytes =
        (uint64{SceneRequestHeaderWords} + uint64{MaxPageRequests} * SceneRequestWords) * sizeof(uint32);
    const bool created =
        create(uint64{slots} * Max(SlotSize, 4096u), "Scene Geometry Pool", _geometry, read) &&
        create(uint64{slots} * SceneSlotClusters * sizeof(SceneGpuCluster), "Scene Cluster Pool", _clusters, read) &&
        create(uint64{MaxMeshes} * sizeof(SceneGpuMesh), "Scene Meshes", _meshTable, read) &&
        create(uint64{MaxNodes} * sizeof(SceneGpuNode), "Scene Nodes", _nodes, read) &&
        create(uint64{MaxGroups} * sizeof(SceneGpuGroup), "Scene Groups", _groups, read) &&
        create(uint64{MaxSections} * sizeof(uint32), "Scene Sections", _sections, read) &&
        create(uint64{MaxPages} * sizeof(uint32), "Scene Page Table", _pageTable, read) &&
        create(uint64{MaxPages} * sizeof(uint32), "Scene Page Requested", _pageRequested, written) &&
        create(uint64{slots} * sizeof(uint32), "Scene Slot Used", _slotUsed, written) &&
        create(requestBytes, "Scene Page Requests", _requests, written);
    if (!created)
    {
        return false;
    }
    for (Readback &readback : _readbacks)
    {
        RhiBufferDesc desc;
        desc.Size = requestBytes + uint64{slots} * sizeof(uint32);
        desc.Usage = RhiBufferUsage::CopyDest;
        desc.Location = RhiMemoryLocation::Readback;
        desc.DebugName = "Scene Streaming Readback";
        Result<RhiBuffer, RhiError> buffer = _device->CreateBuffer(desc);
        if (buffer.IsError())
        {
            MARLIN_LOG(LogRenderer, Error, "The streaming readback could not be created: {0}",
                       GetErrorName(buffer.Error()));
            return false;
        }
        readback.Buffer = buffer.Value();
    }
    return true;
}

void SceneRendererSubsystem::AddPasses(RenderFrame &frame)
{
    if (_textures != nullptr)
    {
        _textures->AddPasses(frame.Graph);
    }
    if (_device == nullptr || _shaders == nullptr || _uploadAllocator == nullptr || _scenes.IsEmpty())
    {
        return;
    }
    _uploadAllocator->EndFrame();
    _frameUploads.Begin(*_uploadAllocator);
    if (!EnsureGeometryBuffers())
    {
        return;
    }
    RenderGraphBuilder &graph = frame.Graph;
    SceneGpuResources resources;
    resources.Geometry = graph.ImportBuffer("Scene Geometry Pool", _geometry);
    resources.Clusters = graph.ImportBuffer("Scene Cluster Pool", _clusters);
    resources.Meshes = graph.ImportBuffer("Scene Meshes", _meshTable);
    resources.Nodes = graph.ImportBuffer("Scene Nodes", _nodes);
    resources.Groups = graph.ImportBuffer("Scene Groups", _groups);
    resources.Sections = graph.ImportBuffer("Scene Sections", _sections);
    resources.PageTable = graph.ImportBuffer("Scene Page Table", _pageTable);
    resources.PageRequested = graph.ImportBuffer("Scene Page Requested", _pageRequested);
    resources.SlotUsed = graph.ImportBuffer("Scene Slot Used", _slotUsed);
    resources.Requests = graph.ImportBuffer("Scene Page Requests", _requests);
    resources.MaxRequests = MaxPageRequests;
    resources.Frame = _renderFrameStamp;
    resources.LodThreshold = _renderLodThreshold;
    const uint32 slots = GetSlotCount();
    const uint64 requestBytes =
        (uint64{SceneRequestHeaderWords} + uint64{MaxPageRequests} * SceneRequestWords) * sizeof(uint32);
    const auto fill = [&](StringView name, RenderGraphBuffer buffer, uint64 size) {
        graph.AddPass(
            name, RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass) { (void)pass.Buffer(buffer, RhiAccess::CopyDest); },
            [buffer, size](RenderGraphContext &context) {
                context.List.FillBuffer(context.GetBuffer(buffer), 0, size, 0);
            });
    };
    if (!_persistentCleared)
    {
        // Stamps start at zero, which no frame uses; a mesh table entry nothing wrote yet draws nothing.
        fill("Scene Clear Page Requested", resources.PageRequested, uint64{MaxPages} * sizeof(uint32));
        fill("Scene Clear Slot Used", resources.SlotUsed, uint64{slots} * sizeof(uint32));
        fill("Scene Clear Meshes", resources.Meshes, uint64{MaxMeshes} * sizeof(SceneGpuMesh));
        _persistentCleared = true;
    }
    fill("Scene Clear Page Requests", resources.Requests, SceneRequestHeaderWords * sizeof(uint32));
    if (!_renderUploads.IsEmpty())
    {
        constexpr uint32 Targets = static_cast<uint32>(GeometryUpload::Target::Count);
        List<SceneUploads::Region> regions[Targets];
        for (const GeometryUpload &upload : _renderUploads)
        {
            const Span<const uint8> bytes = upload.Source != nullptr
                                                ? upload.Source->GetBytes().Slice(upload.SourceOffset, upload.Size)
                                                : upload.Bytes.AsSpan();
            regions[static_cast<uint32>(upload.Into)].Add({upload.Offset, bytes});
        }
        const RenderGraphBuffer targets[Targets] = {resources.Geometry, resources.Clusters, resources.Meshes,
                                                    resources.Nodes,    resources.Groups,   resources.PageTable,
                                                    resources.Sections};
        static constexpr StringView Names[Targets] = {
            "Scene Geometry Upload", "Scene Clusters Upload",   "Scene Meshes Upload",  "Scene Nodes Upload",
            "Scene Groups Upload",   "Scene Page Table Upload", "Scene Sections Upload"};
        for (uint32 target = 0; target < Targets; ++target)
        {
            _frameUploads.UploadTo(graph, Names[target], targets[target], regions[target].AsSpan());
        }
        _renderUploads.Clear();
    }
    // The material table: a slot's permutation and element, and each permutation's parameter blocks.
    {
        List<uint32> slots;
        for (const SceneMaterialFrame::Slot &slot : _renderMaterials.Slots)
        {
            slots.Add(slot.Permutation);
            slots.Add(slot.Element);
        }
        if (slots.IsEmpty())
        {
            slots.Add(~0u);
            slots.Add(0);
        }
        const uint32 noBinding = ~0u;
        resources.MaterialBindings =
            _frameUploads.Upload(graph, "Scene Material Bindings",
                                 _renderBindingSections.IsEmpty()
                                     ? AsBytes(noBinding)
                                     : Span<const uint8>(reinterpret_cast<const uint8 *>(_renderBindingSections.Data()),
                                                         _renderBindingSections.Count() * sizeof(uint32)));
        resources.MaterialSlots = _frameUploads.Upload(
            graph, "Scene Material Slots",
            Span<const uint8>(reinterpret_cast<const uint8 *>(slots.Data()), slots.Count() * sizeof(uint32)));
        for (const SceneMaterialFrame::Permutation &permutation : _renderMaterials.Permutations)
        {
            SceneGpuResources::MaterialPermutation &table = resources.Materials.AddDefaulted();
            table.Shader = permutation.Shader.AsView();
            table.Variant = permutation.Variant.AsSpan();
            table.Stride = permutation.Stride;
            table.CustomBsdfs = permutation.CustomBsdfs.AsSpan();
            table.Count =
                permutation.Stride != 0 ? static_cast<uint32>(permutation.Parameters.Count() / permutation.Stride) : 0;
            table.Parameters = _frameUploads.Upload(graph, "Scene Material Parameters",
                                                    permutation.Parameters.IsEmpty() ? AsBytes(slots[0])
                                                                                     : permutation.Parameters.AsSpan());
        }
    }
    for (RenderScene *scene : _scenes)
    {
        if (scene->GetRenderViews().IsEmpty())
        {
            continue;
        }
        resources.Instances = scene->AddInstancePasses(graph, *_shaders, _frameUploads, *_device);
        resources.Lights = _frameUploads.Upload(graph, "Scene Lights", AsBytes(scene->GetRenderLights()));
        resources.InstanceCount = scene->GetRenderInstanceCount();
        if (!resources.Instances.IsValid())
        {
            continue;
        }
        for (const SceneViewSnapshot &view : scene->GetRenderViews())
        {
            RenderView(frame, view, resources);
        }
    }

    // The pages the views asked for and the slots they used, for the main thread once the GPU is done.
    if (_renderReadback < 3)
    {
        Readback &readback = _readbacks[_renderReadback];
        const RhiBuffer target = readback.Buffer;
        const RenderGraphBuffer requests = resources.Requests;
        const RenderGraphBuffer slotUsed = resources.SlotUsed;
        graph.AddPass(
            "Scene Streaming Readback", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass) {
                (void)pass.Buffer(requests, RhiAccess::CopySource);
                (void)pass.Buffer(slotUsed, RhiAccess::CopySource);
                pass.NeverCull();
            },
            [target, requests, slotUsed, requestBytes, slots](RenderGraphContext &context) {
                context.List.CopyBuffer(target, 0, context.GetBuffer(requests), 0, requestBytes);
                context.List.CopyBuffer(target, requestBytes, context.GetBuffer(slotUsed), 0,
                                        uint64{slots} * sizeof(uint32));
                RhiGlobalBarrier toHost;
                toHost.SyncBefore = RhiSync::Copy;
                toHost.AccessBefore = RhiAccess::CopyDest;
                toHost.SyncAfter = RhiSync::Host;
                toHost.AccessAfter = RhiAccess::HostRead;
                RhiBarriers barriers;
                barriers.Global = Span<const RhiGlobalBarrier>(&toHost, 1);
                context.List.Barrier(barriers);
            });
        readback.Written = true;
        readback.Frame = _renderFrameStamp;
    }
}

void SceneRendererSubsystem::RenderView(RenderFrame &frame, const SceneViewSnapshot &view,
                                        const SceneGpuResources &scene)
{
    SceneViewTextures textures;
    SceneViewHistory &history = view.View->GetState().Get<SceneViewHistory>();
    const SceneGpuView views[2] = {MakeViewData(view), history.Valid ? history.View : MakeViewData(view)};
    textures.ViewData = _frameUploads.Upload(frame.Graph, "Scene Views", AsBytes(views));
    history.View = views[0];
    history.Valid = true;
    SceneViewContext context{frame, frame.Graph, *_device, *_shaders, view, scene, textures};
    for (const Stage &stage : _stages)
    {
        for (SceneViewFeature *feature : stage.Features)
        {
            if (view.Mode == nullptr || view.Mode->UsesFeature(_features.GetType(*feature)))
            {
                feature->AddPasses(context);
            }
        }
    }
    if (textures.SceneColor.IsValid())
    {
        frame.BindImage(view.ImageSlot, {textures.SceneColor, RenderImageRange::Ui});
    }
}

void SceneRendererFrameFeature::Extract(RenderFrameExtract & /*frame*/)
{
    if (SceneRendererSubsystem *renderer = Engine::GetSubsystem<SceneRendererSubsystem>())
    {
        renderer->Extract();
    }
}

void SceneRendererFrameFeature::AddPasses(RenderFrame &frame)
{
    if (SceneRendererSubsystem *renderer = Engine::GetSubsystem<SceneRendererSubsystem>())
    {
        renderer->AddPasses(frame);
    }
}
} // namespace Marlin
