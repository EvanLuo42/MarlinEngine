#pragma once

#include "CoreTypes.h"
#include "EditorShell/DockSpace.h"
#include "EditorShell/EditingContext.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "SceneEditor/SceneContext.gen.h"

namespace Marlin
{
/// Opens Levels and Prefabs in a scene context (Documentation/Architecture/09 §5).
MCLASS()
class SCENEEDITOR_API SceneContextType final : public AssetContextType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetIcon() const override
    {
        return "Scene";
    }
    [[nodiscard]] StringView GetIconColor() const override
    {
        return "Tab.Icon.Asset";
    }
    [[nodiscard]] bool CanEdit(StringView assetType) const override;
    [[nodiscard]] UniquePtr<EditingContext> Create(const Guid &asset, StringView name) const override;
};

/// The editing of a Level or Prefab: a toolbar on top and the scene's docks (SceneDocks.h) in a DockSpace, laid out
/// as the user left them in the last scene. Its World, selection and undo are not written yet.
class SCENEEDITOR_API SceneContext final : public EditingContext
{
public:
    /// *asset* is invalid for a scene that is not saved yet.
    SceneContext(const SceneContextType *type, const Guid &asset, StringView name);
    ~SceneContext() override;

    [[nodiscard]] Guid GetAsset() const override
    {
        return _asset;
    }

    [[nodiscard]] UIElement BuildContent() override;

    [[nodiscard]] DockSpace &GetDocks() const
    {
        return *_docks;
    }

    /// Whether *context* is a scene context.
    [[nodiscard]] static bool IsScene(const EditingContext &context);
    /// The outliner and the details either side of the viewport, the output log under it.
    [[nodiscard]] static DockLayoutState GetDefaultLayout();

private:
    Guid _asset;
    UniquePtr<DockSpace> _docks;
};
} // namespace Marlin
