#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorShell/DockSpace.h"
#include "EditorShell/EditingContext.h"
#include "EditorUI/Signal.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "AssetManager/AssetManagerContext.gen.h"

namespace Marlin
{
class AssetCatalogSubsystem;
struct CatalogAsset;

/// How the browser orders assets.
enum class AssetSort : int32
{
    Name,
    Type,
    /// Newest first.
    Modified,
};

/// What the Asset Manager's docks share: where the user browses, how, and what is selected.
struct AssetBrowserState
{
    /// A folder's virtual path, or StarredFolder or RecentFolder.
    Signal<String> Folder;
    Signal<String> Search;
    /// The source types shown; empty shows every type.
    Signal<List<String>> Types;
    Signal<int32> Sort;
    /// 0 for tiles, 1 for a list.
    Signal<int32> View;
    /// The smallest a tile gets, in painter units.
    Signal<float32> TileSize;
    Signal<Guid> Selected;
    /// The folders closed in the tree; the others are open.
    Signal<List<String>> Collapsed;
};

/// The Asset Manager's tab (Documentation/Architecture/09 §5): always open, first and pinned.
MCLASS()
class ASSETMANAGER_API AssetManagerContextType final : public PinnedContextType
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetIcon() const override
    {
        return "FolderOpen";
    }
    [[nodiscard]] StringView GetIconColor() const override
    {
        return "Tab.Icon.AssetManager";
    }
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Asset Manager";
    }
    [[nodiscard]] UniquePtr<EditingContext> Create() const override;
};

/// The Asset Manager: a toolbar (Import, Create, Save All, Refresh, Find Unused, Validate, Source Control) over the
/// Folders, Browser and Details docks (AssetManagerDocks.h), which browse the AssetCatalogSubsystem.
class ASSETMANAGER_API AssetManagerContext final : public EditingContext
{
public:
    /// Special Folder values.
    static constexpr StringView StarredFolder = "*Starred";
    static constexpr StringView RecentFolder = "*Recent";

    explicit AssetManagerContext(const AssetManagerContextType *type);
    ~AssetManagerContext() override;

    [[nodiscard]] bool IsPinned() const override
    {
        return true;
    }

    [[nodiscard]] UIElement BuildContent() override;

    [[nodiscard]] AssetBrowserState &GetState()
    {
        return _state;
    }

    /// Null when the catalog does not run, such as in tests.
    [[nodiscard]] AssetCatalogSubsystem *GetCatalog() const;

    /// The assets the browser shows, in order: in the folder, matching the search and the types; follows changes.
    [[nodiscard]] const List<Guid> &GetVisible() const
    {
        return _visible.Get();
    }

    /// Opens *uuid* in the editing context for its type.
    void Open(const Guid &uuid);
    /// Selects *uuid* and the folder that holds it, opening the folders above.
    void ShowInFolder(const Guid &uuid);
    /// Selects *uuid* and shows its actions at *position* in the window of *origin*.
    void ShowAssetMenu(const UIElementNode *origin, Vector2 position, const Guid &uuid);

    [[nodiscard]] static bool IsAssetManager(const EditingContext &context);
    /// The folders, the browser and the details side by side.
    [[nodiscard]] static DockLayoutState GetDefaultLayout();

private:
    [[nodiscard]] UIElement BuildToolbar();

    ReactiveOwner *_owner = nullptr;
    AssetBrowserState _state;
    Computed<List<Guid>> _visible;
    UniquePtr<DockSpace> _docks;
};
} // namespace Marlin
