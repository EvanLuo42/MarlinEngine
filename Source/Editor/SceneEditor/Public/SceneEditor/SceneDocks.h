#pragma once

#include "CoreTypes.h"
#include "EditorShell/Dock.h"
#include "SceneEditor/SceneDocks.gen.h"

namespace Marlin
{
/// The docks of scene contexts (SceneContext); empty until their content is written.
MCLASS(Abstract)
class SCENEEDITOR_API SceneDockType : public DockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] bool CanDockIn(const EditingContext &context) const override;
};

/// The scene seen through a camera.
MCLASS()
class SCENEEDITOR_API SceneViewportDock final : public SceneDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Viewport";
    }
};

/// The scene's entities as a tree.
MCLASS()
class SCENEEDITOR_API SceneOutlinerDock final : public SceneDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Outliner";
    }
};

/// The components of the selected entities.
MCLASS()
class SCENEEDITOR_API SceneDetailsDock final : public SceneDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Details";
    }
};

MCLASS()
class SCENEEDITOR_API SceneOutputLogDock final : public SceneDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Output Log";
    }
};
} // namespace Marlin
