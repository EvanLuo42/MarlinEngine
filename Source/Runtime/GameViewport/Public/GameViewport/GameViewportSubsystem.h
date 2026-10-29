#pragma once

#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "SceneRenderer/SceneView.h"
#include "World/EntityQuery.h"
#include "World/System.h"
#include "GameViewport/GameViewportSubsystem.gen.h"

namespace Marlin
{
class World;

/// The game's view (Documentation/Architecture/07 §5.5): a SceneView of the first world that renders, drawn with
/// DrawImage over the whole main window at the start of each frame, so the game's UI draws over it. Its camera is the
/// world's first Camera entity, which GameCameraSystem copies in each frame; without one it looks at the origin.
MCLASS(Dependencies = "WorldManagerSubsystem, UIPainterSubsystem, SceneRendererSubsystem")
class GAMEVIEWPORT_API GameViewportSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    GameViewportSubsystem();
    ~GameViewportSubsystem() override;

    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// The view of *world*, or null if the viewport shows another world or none.
    [[nodiscard]] SceneView *FindView(const World &world) const;

private:
    World *_world = nullptr;
    UniquePtr<SceneView> _view;
};

/// Copies the world's first Camera into the game viewport's SceneView, once the world's transforms are final.
MCLASS(Phase = LatePhase, Sets = (PresentationSet))
class GAMEVIEWPORT_API GameCameraSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    EntityQueryBase _cameras;
};
} // namespace Marlin
