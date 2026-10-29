#pragma once

#include "CoreTypes.h"
#include "EditorShell/Dock.h"
#include "AssetManager/AssetManagerDocks.gen.h"

namespace Marlin
{
/// The docks of the Asset Manager (AssetManagerContext).
MCLASS(Abstract)
class ASSETMANAGER_API AssetManagerDockType : public DockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] bool CanDockIn(const EditingContext &context) const override;
};

/// The content folders as a tree, under Starred and Recently Modified; picks the folder the browser shows.
MCLASS()
class ASSETMANAGER_API AssetFoldersDock final : public AssetManagerDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Folders";
    }
    [[nodiscard]] UIElement Build(EditingContext &context) const override;
};

/// The assets of the folder as tiles or a list, with the path, search, sort and type filters over them.
MCLASS()
class ASSETMANAGER_API AssetBrowserDock final : public AssetManagerDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Browser";
    }
    [[nodiscard]] UIElement Build(EditingContext &context) const override;
};

/// The selected asset: a preview, its actions, properties, tags and what references it.
MCLASS()
class ASSETMANAGER_API AssetDetailsDock final : public AssetManagerDockType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Details";
    }
    [[nodiscard]] UIElement Build(EditingContext &context) const override;
};
} // namespace Marlin
