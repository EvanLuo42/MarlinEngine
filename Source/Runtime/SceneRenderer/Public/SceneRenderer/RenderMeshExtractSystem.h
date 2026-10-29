#pragma once

#include "CoreTypes.h"
#include "World/EntityQuery.h"
#include "World/System.h"
#include "SceneRenderer/RenderMeshExtractSystem.gen.h"

namespace Marlin
{
/// The extract provider of RenderMesh, the first primitive of the GPU Scene (Documentation/Architecture/07 §5.2):
/// gives each entity with a RenderMesh and a WorldTransform an instance slot of its world's RenderScene, writes the
/// instances of chunks whose transform or slot changed (in parallel, by the column versions), and frees the slots of
/// entities that are gone or no longer render. Other primitives (particles, terrain, foliage) are systems like it.
MCLASS(Phase = LatePhase, Sets = (PresentationSet))
class SCENERENDERER_API RenderMeshExtractSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    /// Entities that render and have no slot yet.
    EntityQueryBase _added;
    /// Entities with a slot.
    EntityQueryBase _existing;
};
/// Finds a world's sun and sky (DirectionalLight, SkyLight) for its RenderScene: the first of each; the sun shines
/// along its entity's -Z.
MCLASS(Phase = LatePhase, Sets = (PresentationSet))
class SCENERENDERER_API RenderLightExtractSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    EntityQueryBase _suns;
    EntityQueryBase _skies;
};
} // namespace Marlin
