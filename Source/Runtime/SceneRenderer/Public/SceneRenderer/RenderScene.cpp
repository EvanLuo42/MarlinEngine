#include "SceneRenderer/RenderScene.h"

#include "Engine/Engine.h"
#include "Math/MathUtility.h"
#include "RHI/RhiDevice.h"
#include "RenderComponents/RenderComponents.h"
#include "SceneRenderer/RenderMeshExtractSystem.h"
#include "SceneRenderer/SceneRendererSubsystem.h"
#include "SceneRenderer/SceneShaders.h"
#include "SceneRenderer/SceneView.h"
#include "Shader/ShaderLibrary.h"
#include "World/EntityManager.h"
#include "World/Hierarchy.h"

namespace Marlin
{
namespace
{
/// The instance buffer's least capacity, so a scene always has one to bind.
constexpr uint32 MinInstanceCapacity = 1024;

SceneGpuInstance MakeInstance(const WorldTransform &transform, uint32 mesh, uint32 materials)
{
    const SceneTilePosition position = SceneTilePosition::FromWorld(transform.Translation);
    SceneGpuInstance instance;
    for (uint32 axis = 0; axis < 3; ++axis)
    {
        instance.Tile[axis] = position.Tile[axis];
        instance.Offset[axis] = position.Offset[axis];
    }
    instance.Mesh = mesh;
    instance.Materials = materials;
    instance.Rotation[0] = transform.Rotation.X;
    instance.Rotation[1] = transform.Rotation.Y;
    instance.Rotation[2] = transform.Rotation.Z;
    instance.Rotation[3] = transform.Rotation.W;
    instance.Scale[0] = transform.Scale.X;
    instance.Scale[1] = transform.Scale.Y;
    instance.Scale[2] = transform.Scale.Z;
    return instance;
}
} // namespace

SceneTilePosition SceneTilePosition::FromWorld(const WorldPosition &position)
{
    SceneTilePosition result;
    const float64 coordinates[3] = {position.X, position.Y, position.Z};
    for (uint32 axis = 0; axis < 3; ++axis)
    {
        const float64 tile = Math::Floor(coordinates[axis] / SceneTileSize);
        result.Tile[axis] = static_cast<int32>(Math::Clamp(tile, -2147483648.0, 2147483647.0));
        result.Offset[axis] =
            static_cast<float32>(coordinates[axis] - static_cast<float64>(result.Tile[axis]) * SceneTileSize);
    }
    return result;
}

RenderScene::RenderScene() = default;
RenderScene::~RenderScene() = default;

void RenderScene::Initialize(SubsystemContext & /*context*/)
{
    _renderer = Engine::GetSubsystem<SceneRendererSubsystem>();
    if (_renderer != nullptr)
    {
        _renderer->AddScene(*this);
    }
}

void RenderScene::Deinitialize()
{
    if (_renderer != nullptr)
    {
        _renderer->RemoveScene(*this);
        for (const SceneGpuInstance &instance : _instances)
        {
            _renderer->ReleaseMesh(instance.Mesh);
        }
        Detach();
    }
    _instances.Clear();
    _entities.Clear();
    _freeSlots.Clear();
    _updates.Clear();
}

void RenderScene::Detach()
{
    if (_instanceBuffer.Buffer.IsValid())
    {
        _renderer->GetDevice()->Destroy(_instanceBuffer.Buffer);
        _instanceBuffer = {};
        _instanceBufferCapacity = 0;
    }
    _renderer = nullptr;
}

uint32 RenderScene::AllocateInstance(Entity entity, uint32 mesh)
{
    uint32 slot = 0;
    if (!_freeSlots.IsEmpty())
    {
        slot = _freeSlots.Pop();
    }
    else
    {
        slot = static_cast<uint32>(_instances.Count());
        _instances.AddDefaulted();
        _entities.AddDefaulted();
    }
    _entities[slot] = entity;
    _instances[slot] = SceneGpuInstance();
    _instances[slot].Mesh = mesh;
    return slot;
}

void RenderScene::FreeInstance(uint32 slot)
{
    if (slot >= _instances.Count() || _entities[slot].IsNull())
    {
        return;
    }
    if (_renderer != nullptr)
    {
        _renderer->ReleaseMaterialBinding(_instances[slot].Materials);
        _renderer->ReleaseMesh(_instances[slot].Mesh);
    }
    _entities[slot] = Entity();
    _freeSlots.Add(slot);
    UpdateInstance(slot, SceneGpuInstance());
}

void RenderScene::UpdateInstance(uint32 slot, const SceneGpuInstance &instance)
{
    _instances[slot] = instance;
    const ScopeLock lock(_updateLock);
    SceneGpuInstanceUpdate &update = _updates.AddDefaulted();
    update.Slot = slot;
    update.Instance = instance;
}

void RenderScene::AddView(SceneView &view)
{
    _views.Add(&view);
}

void RenderScene::RemoveView(SceneView &view)
{
    _views.Remove(&view);
}

void RenderScene::SetLights(bool hasSun, const SceneGpuLights &sun, bool hasSky, const SceneGpuLights &sky)
{
    _hasSun = hasSun;
    _hasSky = hasSky;
    MemCopy(_lights.SunDirection, sun.SunDirection, sizeof(_lights.SunDirection));
    MemCopy(_lights.SunIlluminance, sun.SunIlluminance, sizeof(_lights.SunIlluminance));
    MemCopy(_lights.Ambient, sky.Ambient, sizeof(_lights.Ambient));
}

void RenderScene::Extract(const SceneRendererSubsystem &renderer)
{
    // The world's lights, or the defaults for what it lacks.
    _renderLights = _lights;
    if (!_hasSun)
    {
        const Vector3 toSun = Vector3(SceneRendererSubsystem::SunDirection[0], SceneRendererSubsystem::SunDirection[1],
                                      SceneRendererSubsystem::SunDirection[2])
                                  .Normalized();
        _renderLights.SunDirection[0] = toSun.X;
        _renderLights.SunDirection[1] = toSun.Y;
        _renderLights.SunDirection[2] = toSun.Z;
        for (uint32 c = 0; c < 3; ++c)
        {
            _renderLights.SunIlluminance[c] = SceneRendererSubsystem::SunIlluminance;
        }
    }
    if (!_hasSky)
    {
        for (uint32 c = 0; c < 3; ++c)
        {
            _renderLights.Ambient[c] = SceneRendererSubsystem::AmbientIlluminance;
        }
    }
    const uint32 count = static_cast<uint32>(_instances.Count());
    _renderInstanceCount = count;
    _wantedCapacity = Max(_instanceBufferCapacity, MinInstanceCapacity);
    while (_wantedCapacity < count)
    {
        _wantedCapacity *= 2;
    }
    if (_wantedCapacity != _instanceBufferCapacity)
    {
        // A new buffer holds nothing yet: every slot uploads.
        _updates.Clear();
        _renderUpdates.Clear();
        for (uint32 slot = 0; slot < count; ++slot)
        {
            SceneGpuInstanceUpdate &update = _renderUpdates.AddDefaulted();
            update.Slot = slot;
            update.Instance = _instances[slot];
        }
    }
    else
    {
        // After any the render job could not scatter yet.
        _renderUpdates.AddRange(_updates.AsSpan());
        _updates.Clear();
    }
    _renderViews.Clear();
    for (SceneView *view : _views)
    {
        if (view->GetWidth() == 0 || view->GetHeight() == 0)
        {
            continue;
        }
        SceneViewSnapshot &snapshot = _renderViews.AddDefaulted();
        snapshot.View = view;
        snapshot.Camera = view->GetCamera();
        snapshot.Width = view->GetWidth();
        snapshot.Height = view->GetHeight();
        snapshot.Mode = renderer.FindViewMode(view->GetMode());
        snapshot.ImageSlot = view->GetImageSlot();
    }
}

RenderGraphBuffer RenderScene::AddInstancePasses(RenderGraphBuilder &graph, ShaderLibrary &shaders,
                                                 SceneUploads &uploads, RhiDevice &device)
{
    bool cleared = true;
    if (_wantedCapacity > _instanceBufferCapacity || !_instanceBuffer.Buffer.IsValid())
    {
        if (_instanceBuffer.Buffer.IsValid())
        {
            // The RHI frees it once the queues are done with the frames that used it.
            device.Destroy(_instanceBuffer.Buffer);
        }
        RhiBufferDesc desc;
        desc.Size = uint64{_wantedCapacity} * sizeof(SceneGpuInstance);
        desc.Usage = RhiBufferUsage::ShaderResource | RhiBufferUsage::UnorderedAccess | RhiBufferUsage::CopyDest;
        desc.DebugName = "Scene Instances";
        Result<RhiBuffer, RhiError> buffer = device.CreateBuffer(desc);
        if (buffer.IsError())
        {
            MARLIN_LOG(LogRenderer, Error, "The instance buffer of {0} instances could not be created: {1}",
                       _wantedCapacity, GetErrorName(buffer.Error()));
            _instanceBuffer = {};
            _instanceBufferCapacity = 0;
            return {};
        }
        _instanceBuffer = {buffer.Value(), false};
        _instanceBufferCapacity = _wantedCapacity;
        cleared = false;
    }
    const RenderGraphBuffer instances = graph.ImportBuffer("Scene Instances", _instanceBuffer);
    if (!cleared)
    {
        // Every slot SceneInvalidMesh until its update scatters, which waits for the scatter shader to load.
        const uint64 size = uint64{_instanceBufferCapacity} * sizeof(SceneGpuInstance);
        graph.AddPass(
            "Scene Clear Instances", RhiQueueType::Graphics,
            [&](RenderGraphPassBuilder &pass) { (void)pass.Buffer(instances, RhiAccess::CopyDest); },
            [instances, size](RenderGraphContext &context) {
                context.List.FillBuffer(context.GetBuffer(instances), 0, size, SceneInvalidMesh);
            });
    }
    // Until the scatter shader has loaded the updates wait, or a scene that stops changing would never upload them.
    if (_renderUpdates.IsEmpty() || !shaders.IsReady(SceneScatterShader))
    {
        return instances;
    }
    const auto count = static_cast<uint32>(_renderUpdates.Count());
    const RenderGraphBuffer updates =
        uploads.Upload(graph, "Scene Instance Updates",
                       Span<const uint8>(reinterpret_cast<const uint8 *>(_renderUpdates.Data()),
                                         _renderUpdates.Count() * sizeof(SceneGpuInstanceUpdate)));
    struct ScatterData
    {
        RenderGraphView Updates;
        RenderGraphView Instances;
    };
    ShaderLibrary *library = &shaders;
    graph.AddPass<ScatterData>(
        "Scene Scatter Instances", RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass, ScatterData &data) {
            data.Updates = pass.ReadBuffer(updates, {0, 0, sizeof(SceneGpuInstanceUpdate)});
            data.Instances = pass.WriteBuffer(instances, {0, 0, sizeof(SceneGpuInstance)});
        },
        [library, count](const ScatterData &data, RenderGraphContext &context) {
            SceneScatterArguments arguments;
            arguments.Updates = context.GetSpan(data.Updates);
            arguments.Instances = context.GetSpan(data.Instances);
            arguments.UpdateCount = count;
            (void)library->DispatchThreads(context.List, SceneScatterShader, arguments, count);
        });
    _renderUpdates.Clear();
    return instances;
}

void RenderMeshExtractSystem::Configure(SystemBuilder &builder)
{
    EntityQueryDesc added;
    added.Read.Add(Components::IdOf<RenderMesh>());
    added.Read.Add(Components::IdOf<WorldTransform>());
    added.None.Add(Components::IdOf<RenderInstance>());
    _added.SetDesc(Move(added));
    builder.AddQuery(_added);
    EntityQueryDesc existing;
    existing.Read.Add(Components::IdOf<RenderMesh>());
    existing.Read.Add(Components::IdOf<WorldTransform>());
    existing.Read.Add(Components::IdOf<RenderInstance>());
    _existing.SetDesc(Move(existing));
    builder.AddQuery(_existing);
}

void RenderMeshExtractSystem::Execute(SystemContext &context)
{
    RenderScene *scene = context.GetWorld().GetSubsystem<RenderScene>();
    SceneRendererSubsystem *renderer = scene != nullptr ? scene->GetRenderer() : nullptr;
    if (scene == nullptr || renderer == nullptr)
    {
        return;
    }
    EntityManager &entities = context.GetEntities();
    CommandBuffer &commands = context.GetCommands();

    // Slots whose entity is gone or no longer renders.
    for (uint32 slot = 0; slot < scene->GetInstanceCapacity(); ++slot)
    {
        const Entity entity = scene->GetInstanceEntity(slot);
        if (entity.IsNull())
        {
            continue;
        }
        const bool exists = entities.Exists(entity);
        if (exists && entities.Has<RenderMesh>(entity) && entities.Has<WorldTransform>(entity))
        {
            continue;
        }
        if (exists && entities.Has<RenderInstance>(entity))
        {
            commands.Remove<RenderInstance>(entity);
        }
        scene->FreeInstance(slot);
    }

    // New entities, one at a time: slots come from one free list.
    _added.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        const RenderMesh *mesh = chunk.Get<RenderMesh>();
        const Span<const WorldTransform> transforms = chunk.Read<WorldTransform>();
        const Span<const Entity> rows = chunk.GetEntities();
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            if (!chunk.IsIncluded(row))
            {
                continue;
            }
            const uint32 index = renderer->AcquireMesh(mesh->Mesh);
            const uint32 materials = renderer->AcquireMaterialBinding(index, Span<const SoftAssetRef<Material>>(mesh->Materials));
            const uint32 slot = scene->AllocateInstance(rows[row], index);
            scene->UpdateInstance(slot, MakeInstance(transforms[row], index, materials));
            commands.Add<RenderInstance>(rows[row], RenderInstance{slot});
        }
    });

    // Entities that moved, or whose slot or mesh changed, chunk by chunk in parallel: each writes its own slots.
    const uint32 lastVersion = context.GetVersions().LastVersion;
    const ComponentId transformId = Components::IdOf<WorldTransform>();
    const ComponentId instanceId = Components::IdOf<RenderInstance>();
    SpinLock strayLock;
    List<Entity> strays;
    _existing.ParallelForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        if (!chunk.HasChanged(transformId, lastVersion) && !chunk.HasChanged(instanceId, lastVersion))
        {
            return;
        }
        const RenderMesh *mesh = chunk.Get<RenderMesh>();
        const Span<const WorldTransform> transforms = chunk.Read<WorldTransform>();
        const Span<const RenderInstance> slots = chunk.Read<RenderInstance>();
        const Span<const Entity> rows = chunk.GetEntities();
        // Held for the chunk, so moving each row's reference never drops the mesh or binding to zero.
        const uint32 index = renderer->AcquireMesh(mesh->Mesh);
        const uint32 materials = renderer->AcquireMaterialBinding(index, Span<const SoftAssetRef<Material>>(mesh->Materials));
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            if (!chunk.IsIncluded(row))
            {
                continue;
            }
            const uint32 slot = slots[row].Slot;
            if (slot >= scene->GetInstanceCapacity() || scene->GetInstanceEntity(slot) != rows[row])
            {
                // A copy of another entity's slot, or one from another world: it gets its own below.
                const ScopeLock lock(strayLock);
                strays.Add(rows[row]);
                continue;
            }
            const uint32 previous = scene->GetInstanceMesh(slot);
            if (previous != index)
            {
                renderer->AddMeshReference(index);
                if (previous != SceneInvalidMesh)
                {
                    renderer->ReleaseMesh(previous);
                }
            }
            const uint32 previousMaterials = scene->GetInstanceMaterials(slot);
            if (previousMaterials != materials)
            {
                renderer->AddMaterialBindingReference(materials);
                renderer->ReleaseMaterialBinding(previousMaterials);
            }
            scene->UpdateInstance(slot, MakeInstance(transforms[row], index, materials));
        }
        renderer->ReleaseMaterialBinding(materials);
        renderer->ReleaseMesh(index);
    });
    for (const Entity entity : strays)
    {
        const RenderMesh *mesh = entities.Get<RenderMesh>(entity);
        const WorldTransform *transform = entities.Get<WorldTransform>(entity);
        if (mesh == nullptr || transform == nullptr)
        {
            continue;
        }
        const uint32 index = renderer->AcquireMesh(mesh->Mesh);
        const uint32 materials = renderer->AcquireMaterialBinding(index, Span<const SoftAssetRef<Material>>(mesh->Materials));
        const uint32 slot = scene->AllocateInstance(entity, index);
        scene->UpdateInstance(slot, MakeInstance(*transform, index, materials));
        commands.Set<RenderInstance>(entity, RenderInstance{slot});
    }
}

void RenderLightExtractSystem::Configure(SystemBuilder &builder)
{
    EntityQueryDesc suns;
    suns.Read.Add(Components::IdOf<DirectionalLight>());
    suns.Read.Add(Components::IdOf<WorldTransform>());
    _suns.SetDesc(Move(suns));
    builder.AddQuery(_suns);
    EntityQueryDesc skies;
    skies.Read.Add(Components::IdOf<SkyLight>());
    _skies.SetDesc(Move(skies));
    builder.AddQuery(_skies);
}

void RenderLightExtractSystem::Execute(SystemContext &context)
{
    RenderScene *scene = context.GetWorld().GetSubsystem<RenderScene>();
    if (scene == nullptr)
    {
        return;
    }
    bool hasSun = false;
    bool hasSky = false;
    SceneGpuLights sun;
    SceneGpuLights sky;
    _suns.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        const Span<const DirectionalLight> lights = chunk.Read<DirectionalLight>();
        const Span<const WorldTransform> transforms = chunk.Read<WorldTransform>();
        for (uint32 row = 0; row < chunk.GetCount() && !hasSun; ++row)
        {
            if (!chunk.IsIncluded(row))
            {
                continue;
            }
            // It shines along -Z, so the direction towards it is +Z.
            const Vector3 toSun = transforms[row].Rotation.Rotate(Vector3(0, 0, 1)).Normalized();
            sun.SunDirection[0] = toSun.X;
            sun.SunDirection[1] = toSun.Y;
            sun.SunDirection[2] = toSun.Z;
            sun.SunIlluminance[0] = lights[row].Color.X * lights[row].Illuminance;
            sun.SunIlluminance[1] = lights[row].Color.Y * lights[row].Illuminance;
            sun.SunIlluminance[2] = lights[row].Color.Z * lights[row].Illuminance;
            hasSun = true;
        }
    });
    _skies.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        const Span<const SkyLight> lights = chunk.Read<SkyLight>();
        for (uint32 row = 0; row < chunk.GetCount() && !hasSky; ++row)
        {
            if (!chunk.IsIncluded(row))
            {
                continue;
            }
            sky.Ambient[0] = lights[row].Color.X * lights[row].Illuminance;
            sky.Ambient[1] = lights[row].Color.Y * lights[row].Illuminance;
            sky.Ambient[2] = lights[row].Color.Z * lights[row].Illuminance;
            hasSky = true;
        }
    });
    scene->SetLights(hasSun, sun, hasSky, sky);
}
} // namespace Marlin
