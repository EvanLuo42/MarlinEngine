#include "SceneRenderer/SceneView.h"

#include "Engine/Engine.h"
#include "Renderer/RendererSubsystem.h"
#include "SceneRenderer/RenderScene.h"
#include "SceneRenderer/SceneViewFeature.h"

namespace Marlin
{
SceneView::SceneView(RenderScene &scene) : _scene(&scene), _state(MakeUnique<SceneViewState>())
{
    if (RendererSubsystem *renderer = Engine::GetSubsystem<RendererSubsystem>())
    {
        _slot = renderer->AllocateImageSlot();
    }
    _scene->AddView(*this);
}

SceneView::~SceneView()
{
    RendererSubsystem *renderer = Engine::GetSubsystem<RendererSubsystem>();
    if (renderer != nullptr)
    {
        // The render job in flight may still render the view and use its state.
        renderer->WaitForRender();
    }
    _scene->RemoveView(*this);
    if (renderer != nullptr && _slot != 0)
    {
        renderer->FreeImageSlot(_slot);
    }
}
} // namespace Marlin
