#include "AssetManager/AssetManagerContext.h"

#include "Application/PlatformApplication.h"
#include "AssetManager/AssetCatalog.h"
#include "EditorShell/EditingContextSubsystem.h"
#include "EditorUI/UIControls.h"
#include "Misc/DateTime.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
/// Recently Modified: changed within this many days.
constexpr int64 RecentDays = 7;

bool ContainsIgnoringCase(StringView text, StringView part)
{
    if (part.IsEmpty())
    {
        return true;
    }
    for (usize start = 0; start + part.Length() <= text.Length(); ++start)
    {
        if (text.Substring(start, part.Length()).EqualsIgnoreCase(part))
        {
            return true;
        }
    }
    return false;
}
} // namespace

UniquePtr<EditingContext> AssetManagerContextType::Create() const
{
    return MakeUnique<AssetManagerContext>(this);
}

AssetManagerContext::AssetManagerContext(const AssetManagerContextType *type) : EditingContext(type)
{
    Title.Set("Asset Manager");
    _owner = Reactive::GetOwner();
    _state.Folder = CreateSignal(String("/Project/Content"));
    _state.Search = CreateSignal(String());
    _state.Types = CreateSignal(List<String>());
    _state.Sort = CreateSignal<int32>(0);
    _state.View = CreateSignal<int32>(0);
    _state.TileSize = CreateSignal<float32>(104);
    _state.Selected = CreateSignal(Guid());
    _state.Collapsed = CreateSignal(List<String>());
    AssetBrowserState *state = &_state;
    _visible = CreateComputed<List<Guid>>([this, state] {
        List<Guid> visible;
        AssetCatalogSubsystem *catalog = GetCatalog();
        if (catalog == nullptr)
        {
            return visible;
        }
        const String &folder = state->Folder.Get();
        const String &search = state->Search.Get();
        const List<String> &types = state->Types.Get();
        const int64 recent = DateTime::UtcNow().GetTicks() - RecentDays * TimeSpan::TicksPerDay;
        List<const CatalogAsset *> matches;
        for (const CatalogAsset &asset : catalog->GetAssets())
        {
            const bool inFolder = folder == StarredFolder ? catalog->IsStarred(asset.Uuid)
                                  : folder == RecentFolder
                                      ? asset.Modified >= recent
                                      : asset.Folder == folder || (asset.Folder.StartsWith(folder.AsView()) &&
                                                                   asset.Folder.Length() > folder.Length() &&
                                                                   asset.Folder[folder.Length()] == '/');
            if (!inFolder || (!types.IsEmpty() && !types.Contains(asset.Type)))
            {
                continue;
            }
            bool found = ContainsIgnoringCase(asset.Name.AsView(), search.AsView());
            for (usize tag = 0; !found && tag < asset.Tags.Count(); ++tag)
            {
                found = ContainsIgnoringCase(asset.Tags[tag].AsView(), search.AsView());
            }
            if (found)
            {
                matches.Add(&asset);
            }
        }
        // The catalog keeps them by name; the other orders sort stably on top of that.
        switch (static_cast<AssetSort>(state->Sort.Get()))
        {
        case AssetSort::Name:
            break;
        case AssetSort::Type:
            matches.StableSort([](const CatalogAsset *a, const CatalogAsset *b) {
                return AssetCatalogSubsystem::GetStyle(a->Type).Label.AsView() <
                       AssetCatalogSubsystem::GetStyle(b->Type).Label.AsView();
            });
            break;
        case AssetSort::Modified:
            matches.StableSort([](const CatalogAsset *a, const CatalogAsset *b) { return a->Modified > b->Modified; });
            break;
        }
        for (const CatalogAsset *asset : matches)
        {
            visible.Add(asset->Uuid);
        }
        return visible;
    });
}

AssetManagerContext::~AssetManagerContext() = default;

AssetCatalogSubsystem *AssetManagerContext::GetCatalog() const
{
    return Editor::GetSubsystem<AssetCatalogSubsystem>();
}

UIElement AssetManagerContext::BuildContent()
{
    // Made the first time the tab shows, once every editor subsystem the docks use is up; under the context's own
    // owner, which outlives the content.
    if (_docks == nullptr)
    {
        Reactive::RunInOwner(_owner, [this] {
            _docks = MakeUnique<DockSpace>(*this, "AssetManager", GetDefaultLayout());
            _docks->SetToolbar([this] { return BuildToolbar(); });
        });
    }
    return _docks->BuildMainArea();
}

UIElement AssetManagerContext::BuildToolbar()
{
    // Only Refresh does something yet; the others wait for their commands (09 §4.3).
    AssetCatalogSubsystem *catalog = GetCatalog();
    return Row({
                   ToolbarButton(
                       "Download", "Import", [] {}, false),
                   ToolbarButton(
                       "Plus", "Create", [] {}, false),
                   ToolbarButton(
                       "SaveAll", "Save All", [] {}, false),
                   ToolbarButton(
                       "RefreshCw", "Refresh",
                       [catalog] {
                           if (catalog != nullptr)
                           {
                               catalog->Rescan();
                           }
                       },
                       catalog != nullptr),
                   ToolbarButton(
                       "ScanSearch", "Find Unused", [] {}, false),
                   ToolbarButton(
                       "ShieldCheck", "Validate", [] {}, false, "Toolbar.ValidateIcon"),
                   ToolbarButton(
                       "GitBranch", "Source Control", [] {}, false, "Toolbar.SourceControlIcon"),
               })
        .Grow(1)
        .Gap(2)
        .AlignItems(UIAlign::Start)
        .Padding(UIEdges(6, 4, 6, 4));
}

void AssetManagerContext::Open(const Guid &uuid)
{
    AssetCatalogSubsystem *catalog = GetCatalog();
    auto *contexts = Editor::GetSubsystem<EditingContextSubsystem>();
    const CatalogAsset *asset = catalog != nullptr ? catalog->Find(uuid) : nullptr;
    if (asset != nullptr && contexts != nullptr)
    {
        (void)contexts->OpenAsset(asset->Uuid, asset->Type.AsView(), asset->Name.AsView());
    }
}

void AssetManagerContext::ShowInFolder(const Guid &uuid)
{
    AssetCatalogSubsystem *catalog = GetCatalog();
    const CatalogAsset *asset = catalog != nullptr ? catalog->Find(uuid) : nullptr;
    if (asset == nullptr)
    {
        return;
    }
    const String folder = asset->Folder;
    // Open every folder down to it.
    _state.Collapsed.Update([&folder](List<String> &collapsed) {
        collapsed.RemoveAll([&folder](const String &path) {
            return folder == path || (folder.StartsWith(path.AsView()) && folder.Length() > path.Length() &&
                                      folder[path.Length()] == '/');
        });
    });
    _state.Folder.Set(folder);
    _state.Selected.Set(uuid);
}

void AssetManagerContext::ShowAssetMenu(const UIElementNode *origin, Vector2 position, const Guid &uuid)
{
    AssetCatalogSubsystem *catalog = GetCatalog();
    const CatalogAsset *asset = catalog != nullptr ? catalog->Find(uuid) : nullptr;
    if (asset == nullptr)
    {
        return;
    }
    _state.Selected.Set(uuid);
    const bool starred = catalog->IsStarred(uuid);
    const String uuidText = Format("{0}", uuid);
    const String path = asset->File;
    List<UIMenuItem> items;
    items.Add({"Open", "ExternalLink", "Enter", [this, uuid] { Open(uuid); }});
    items.Add({"Show in Folder", "FolderOpen", {}, [this, uuid] { ShowInFolder(uuid); }});
    items.Add(MenuSeparator());
    items.Add({"Copy GUID", "Copy", {}, [uuidText] { (void)PlatformApplication::SetClipboardText(uuidText.CStr()); }});
    items.Add({"Copy Path", "Copy", {}, [path] { (void)PlatformApplication::SetClipboardText(path.CStr()); }});
    items.Add(MenuSeparator());
    items.Add(
        {starred ? "Unstar" : "Star", "Star", {}, [catalog, uuid, starred] { catalog->SetStarred(uuid, !starred); }});
    items.Add(MenuSeparator());
    items.Add({"Rename", "Pencil", "F2", {}, false});
    items.Add({"Delete", "Trash", "Del", {}, false});
    ::Marlin::ShowContextMenu(origin, position, Move(items));
}

bool AssetManagerContext::IsAssetManager(const EditingContext &context)
{
    return context.GetType() != nullptr && context.GetType()->GetType().IsA(TypeOf<AssetManagerContextType>());
}

DockLayoutState AssetManagerContext::GetDefaultLayout()
{
    DockLayoutState layout;
    DockLayout::AddStack(layout.Main.Top, -1, {DockLayout::Toolbar});
    List<DockNodeState> &body = layout.Main.Body;
    const int32 row = DockLayout::AddSplit(body, -1, DockAxis::Row);
    DockLayout::AddStack(body, row, {"AssetFoldersDock"}, 0.16f);
    DockLayout::AddStack(body, row, {"AssetBrowserDock"}, 0.64f);
    DockLayout::AddStack(body, row, {"AssetDetailsDock"}, 0.2f);
    return layout;
}
} // namespace Marlin
