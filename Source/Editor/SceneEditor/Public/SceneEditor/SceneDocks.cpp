#include "SceneEditor/SceneDocks.h"

#include "SceneEditor/SceneContext.h"

namespace Marlin
{
bool SceneDockType::CanDockIn(const EditingContext &context) const
{
    return SceneContext::IsScene(context);
}
} // namespace Marlin
