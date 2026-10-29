#include "SceneEditor/SceneContext.h"

namespace Marlin
{
bool SceneContextType::CanEdit(StringView assetType) const
{
    return assetType == "Level" || assetType == "Prefab";
}

UniquePtr<EditingContext> SceneContextType::Create(const Guid &asset, StringView name) const
{
    return MakeUnique<SceneContext>(this, asset, name);
}

SceneContext::SceneContext(const SceneContextType *type, const Guid &asset, StringView name)
    : EditingContext(type), _asset(asset)
{
    Title.Set(String(name));
    _docks = MakeUnique<DockSpace>(*this, "Scene", GetDefaultLayout());
}

SceneContext::~SceneContext() = default;

UIElement SceneContext::BuildContent()
{
    return _docks->BuildMainArea();
}

bool SceneContext::IsScene(const EditingContext &context)
{
    return context.GetType() != nullptr && context.GetType()->GetType().IsA(TypeOf<SceneContextType>());
}

DockLayoutState SceneContext::GetDefaultLayout()
{
    DockLayoutState layout;
    DockLayout::AddStack(layout.Main.Top, -1, {DockLayout::Toolbar});
    List<DockNodeState> &body = layout.Main.Body;
    const int32 row = DockLayout::AddSplit(body, -1, DockAxis::Row);
    DockLayout::AddStack(body, row, {"SceneOutlinerDock"}, 0.2f);
    const int32 middle = DockLayout::AddSplit(body, row, DockAxis::Column, 0.58f);
    DockLayout::AddStack(body, middle, {"SceneViewportDock"}, 0.72f);
    DockLayout::AddStack(body, middle, {"SceneOutputLogDock"}, 0.28f);
    DockLayout::AddStack(body, row, {"SceneDetailsDock"}, 0.22f);
    return layout;
}
} // namespace Marlin
