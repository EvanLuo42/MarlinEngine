#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiResources.h"
#include "RenderGraph/RenderGraphResources.h"
#include "SceneRenderer/SceneGpuTypes.h"
#include "SceneRenderer/SceneViewFeature.h"
#include "World/Entity.h"
#include "World/World.h"
#include "SceneRenderer/RenderScene.gen.h"

namespace Marlin
{
class RhiDevice;
class SceneRendererSubsystem;
class SceneUploads;
class SceneView;
class ShaderLibrary;

/// The GPU Scene of one World (Documentation/Architecture/07 §5.1, §5.2): the instance buffer every SceneView of the
/// world draws from, and the views themselves. Each renderable entity holds a stable instance slot (RenderInstance)
/// from a free list; the primitives' extract systems write the instances that changed, and Extract hands them to the
/// render job, which scatters them into the persistent instance buffer. Only what changed is uploaded.
MCLASS(Sets = (PresentationSet))
class SCENERENDERER_API RenderScene final : public WorldSubsystem
{
    MGENERATED_BODY()
public:
    RenderScene();
    ~RenderScene() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    /// The renderer the scene draws with; null once it has shut down, before or without the world.
    [[nodiscard]] SceneRendererSubsystem *GetRenderer() const
    {
        return _renderer;
    }
    /// Called by the renderer as it shuts down: frees the GPU side and stops referencing it.
    void Detach();

    // Instances, for the primitives' extract systems while the world updates.

    /// A free slot for *entity*, holding *mesh* (a SceneRendererSubsystem geometry index the scene now references).
    [[nodiscard]] uint32 AllocateInstance(Entity entity, uint32 mesh);
    /// Frees *slot* and drops its mesh and material binding references.
    void FreeInstance(uint32 slot);
    /// Writes *instance* to *slot* this frame; changing its mesh moves the reference. Thread-safe.
    void UpdateInstance(uint32 slot, const SceneGpuInstance &instance);
    [[nodiscard]] Entity GetInstanceEntity(uint32 slot) const
    {
        return slot < _entities.Count() ? _entities[slot] : Entity();
    }
    [[nodiscard]] uint32 GetInstanceMesh(uint32 slot) const
    {
        return slot < _instances.Count() ? _instances[slot].Mesh : SceneInvalidMesh;
    }
    /// The material binding of *slot*'s instance (SceneGpuInstance::Materials), whose reference it holds.
    [[nodiscard]] uint32 GetInstanceMaterials(uint32 slot) const
    {
        return slot < _instances.Count() ? _instances[slot].Materials : 0;
    }
    /// The slots in use or free, the bound of every slot.
    [[nodiscard]] uint32 GetInstanceCapacity() const
    {
        return static_cast<uint32>(_instances.Count());
    }

    // Views, main thread.

    [[nodiscard]] Span<SceneView *const> GetViews() const
    {
        return _views.AsSpan();
    }

    // The render side, driven by SceneRendererSubsystem.

    /// Main thread, between frames: hands this frame's changes and views to the render job.
    void Extract(const SceneRendererSubsystem &renderer);
    /// The world's sun and sky as RenderLightExtractSystem found them; a world without them gets the defaults
    /// (SceneRendererSubsystem::SunIlluminance and AmbientIlluminance).
    void SetLights(bool hasSun, const SceneGpuLights &sun, bool hasSky, const SceneGpuLights &sky);
    /// The lights the render job draws with, from Extract.
    [[nodiscard]] const SceneGpuLights &GetRenderLights() const
    {
        return _renderLights;
    }
    /// Render job: imports the instance buffer into *graph* and scatters the changed instances into it; invalid if
    /// the scene has no instances.
    [[nodiscard]] RenderGraphBuffer AddInstancePasses(RenderGraphBuilder &graph, ShaderLibrary &shaders,
                                                      SceneUploads &uploads, RhiDevice &device);

    [[nodiscard]] Span<const SceneViewSnapshot> GetRenderViews() const
    {
        return _renderViews.AsSpan();
    }

    [[nodiscard]] uint32 GetRenderInstanceCount() const
    {
        return _renderInstanceCount;
    }

private:
    friend class SceneView;

    void AddView(SceneView &view);
    void RemoveView(SceneView &view);

    SceneRendererSubsystem *_renderer = nullptr;

    // Main thread and the extract systems.
    List<SceneGpuInstance> _instances;
    List<Entity> _entities;
    List<uint32> _freeSlots;
    SpinLock _updateLock;
    List<SceneGpuInstanceUpdate> _updates;
    List<SceneView *> _views;

    // Render job.
    List<SceneGpuInstanceUpdate> _renderUpdates;
    List<SceneViewSnapshot> _renderViews;
    SceneGpuLights _lights;
    bool _hasSun = false;
    bool _hasSky = false;
    SceneGpuLights _renderLights;
    uint32 _renderInstanceCount = 0;
    RenderGraphExternalBuffer _instanceBuffer;
    uint32 _instanceBufferCapacity = 0;
    /// What Extract decided the instance buffer must hold; larger than its capacity when it grows this frame.
    uint32 _wantedCapacity = 0;
    /// A buffer that grew out, destroyed once the frame that replaced it is recorded.
    RhiBuffer _retiredBuffer;
};
} // namespace Marlin
