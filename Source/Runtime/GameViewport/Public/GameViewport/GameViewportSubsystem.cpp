#include "GameViewport/GameViewportSubsystem.h"

#include "Engine/Engine.h"
#include "Math/Vector.h"
#include "Renderer/RenderSurface.h"
#include "RenderComponents/RenderComponents.h"
#include "SceneRenderer/RenderScene.h"
#include "UIPainter/UIBrush.h"
#include "UIPainter/UIPainter.h"
#include "UIPainterRenderer/UIPainterSubsystem.h"
#include "World/Hierarchy.h"
#include "World/WorldManagerSubsystem.h"

namespace Marlin
{
GameViewportSubsystem::GameViewportSubsystem() = default;
GameViewportSubsystem::~GameViewportSubsystem() = default;

void GameViewportSubsystem::Deinitialize()
{
    _view.Reset();
    _world = nullptr;
}

SceneView *GameViewportSubsystem::FindView(const World &world) const
{
    return _world == &world ? _view.Get() : nullptr;
}

void GameViewportSubsystem::BeginFrame(const FrameTime & /*time*/)
{
    WorldManagerSubsystem *worlds = Engine::GetSubsystem<WorldManagerSubsystem>();
    UIPainterSubsystem *painter = Engine::GetSubsystem<UIPainterSubsystem>();
    UIWindowCanvas *canvas = painter != nullptr ? painter->GetMainCanvas() : nullptr;
    if (worlds == nullptr || canvas == nullptr)
    {
        return;
    }
    // The first world that renders; a view follows it until it goes.
    World *shown = nullptr;
    for (const UniquePtr<World> &world : worlds->GetWorlds())
    {
        if (world->GetSubsystem<RenderScene>() != nullptr)
        {
            shown = world.Get();
            break;
        }
    }
    if (shown != _world)
    {
        _view.Reset();
        _world = shown;
        if (_world != nullptr)
        {
            _view = MakeUnique<SceneView>(*_world->GetSubsystem<RenderScene>());
            SceneCamera camera;
            camera.Position = WorldPosition(0, 2, 8);
            camera.Rotation = Quaternion::FromAxisAngle(Vector3::Right, -0.24f);
            _view->SetCamera(camera);
        }
    }
    if (_view == nullptr)
    {
        return;
    }
    const RenderSurface &surface = canvas->GetSurface();
    _view->SetSize(surface.GetWidth(), surface.GetHeight());
    const float32 scale = surface.GetDpiScale() > 0 ? surface.GetDpiScale() : 1.0f;
    canvas->GetPainter().DrawImage(UIImage::FromFrameSlot(_view->GetImageSlot()),
                                   {0, 0, static_cast<float32>(surface.GetWidth()) / scale,
                                    static_cast<float32>(surface.GetHeight()) / scale});
}

void GameCameraSystem::Configure(SystemBuilder &builder)
{
    EntityQueryDesc desc;
    desc.Read.Add(Components::IdOf<Camera>());
    desc.Read.Add(Components::IdOf<WorldTransform>());
    _cameras.SetDesc(Move(desc));
    builder.AddQuery(_cameras);
}

void GameCameraSystem::Execute(SystemContext &context)
{
    const GameViewportSubsystem *viewport = Engine::GetSubsystem<GameViewportSubsystem>();
    SceneView *view = viewport != nullptr ? viewport->FindView(context.GetWorld()) : nullptr;
    if (view == nullptr)
    {
        return;
    }
    bool found = false;
    _cameras.ForEachChunk([&](ChunkView &chunk) {
        const Span<const Camera> cameras = chunk.Read<Camera>();
        const Span<const WorldTransform> transforms = chunk.Read<WorldTransform>();
        for (uint32 row = 0; row < chunk.GetCount() && !found; ++row)
        {
            if (!chunk.IsIncluded(row))
            {
                continue;
            }
            SceneCamera camera;
            camera.Position = transforms[row].Translation;
            camera.Rotation = transforms[row].Rotation;
            camera.VerticalFov = cameras[row].VerticalFov;
            camera.Near = cameras[row].Near;
            view->SetCamera(camera);
            found = true;
        }
    });
}
} // namespace Marlin
