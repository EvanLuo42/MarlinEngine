#include "AssetManager/AssetManagerDocks.h"

#include "Application/PlatformApplication.h"
#include "AssetManager/AssetCatalog.h"
#include "AssetManager/AssetManagerContext.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/UIAnimation.h"
#include "EditorUI/UIControls.h"
#include "EditorUI/UIWidgets.h"
#include "Engine/Engine.h"
#include "Misc/DateTime.h"
#include "String/Format.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
namespace
{
constexpr UITransition RowFade{0.08f, &Easing::EaseOutCubic};

AssetManagerContext &AssetManagerOf(EditingContext &context)
{
    return static_cast<AssetManagerContext &>(context);
}

Span<const UIFontFace> Monospace()
{
    const UITextSubsystem *text = Engine::GetSubsystem<UITextSubsystem>();
    return text != nullptr ? text->GetMonospaceFaces() : Span<const UIFontFace>();
}

/// The type's icon in its color.
UIElement TypeIcon(StringView type, float32 size, float32 strokeWidth = 1.5f)
{
    const AssetTypeStyle &style = AssetCatalogSubsystem::GetStyle(type);
    return IconView(style.Icon.AsView(), ThemeColor(style.Color.AsView()), size, strokeWidth);
}

String FormatSize(uint64 bytes)
{
    if (bytes < 1024)
    {
        return Format("{0} B", bytes);
    }
    if (bytes < 1024 * 1024)
    {
        return Format("{0} KB", (bytes + 512) / 1024);
    }
    const uint64 tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
    return Format("{0}.{1} MB", tenths / 10, tenths % 10);
}

String FormatDate(int64 ticks)
{
    if (ticks <= 0)
    {
        return String("-");
    }
    const DateTime date(ticks);
    return Format("{0}-{1}{2}-{3}{4}", date.Year(), date.Month() < 10 ? "0" : "", date.Month(),
                  date.Day() < 10 ? "0" : "", date.Day());
}

/// The path the user sees: /Project/Content/Rocks/Rock.masset shows as Content/Rocks/Rock.
String DisplayPath(StringView path)
{
    StringView shown = path;
    if (shown.StartsWith("/Project/"))
    {
        shown = shown.Substring(9);
    }
    else if (shown.StartsWith("/Engine/Content"))
    {
        shown = shown.Substring(8);
        return Format("Engine/{0}", shown.EndsWith(".masset") ? shown.Left(shown.Length() - 7) : shown);
    }
    return String(shown.EndsWith(".masset") ? shown.Left(shown.Length() - 7) : shown);
}

/// A small gray heading of the folder tree, such as FAVORITES.
UIElement GroupLabel(StringView text)
{
    return Box({Text(String(text)).FontSize(ThemeMetric("FontSize.Small")).TextColor(ThemeColor("Tree.Group"))})
        .Height(ThemeMetric("Tree.RowHeight"))
        .Justify(UIJustify::Center)
        .Padding(UIEdges(10, 0));
}

/// A row of the folder tree: *depth* indents it; *expandable* gives it a caret that runs *toggle*.
UIElement TreeRow(uint32 depth, Function<bool()> selected, UIElement icon, UIValue<String> label, UIValue<String> count,
                  Function<void()> select, bool expandable, Function<bool()> expanded, Function<void()> toggle)
{
    auto *callbacks = new Function<void()>[2]{Move(select), Move(toggle)};
    Reactive::OnCleanup([callbacks] { delete[] callbacks; });
    auto *checks = new Function<bool()>[2]{Move(selected), Move(expanded)};
    Reactive::OnCleanup([checks] { delete[] checks; });
    UIElement caret = Box().Width(10).Height(10).Shrink(0);
    if (expandable)
    {
        caret =
            WithChildren(Move(caret), {IconView("ChevronDown", ThemeColor("Tree.Caret"), 10, 1.5f).Visible([checks] {
                                           return checks[1]();
                                       }),
                                       IconView("ChevronRight", ThemeColor("Tree.Caret"), 10, 1.5f).Visible([checks] {
                                           return !checks[1]();
                                       })})
                .OnPointerDown([callbacks](UIPointerEvent &event) {
                    if (event.Button == MouseButton::Left)
                    {
                        event.Handled = true;
                        callbacks[1]();
                    }
                });
    }
    UIElement row = Row();
    const Signal<bool> hovered = TrackHover(row);
    return WithChildren(Move(row), {caret, icon,
                                    Box({Text(Move(label)).FontSize(ThemeMetric("FontSize.Normal")).TextColor([checks] {
                                        return ThemeColor(checks[0]() ? "Tree.SelectedText" : "Tree.Text")();
                                    })})
                                        .Grow(1)
                                        .Clip(),
                                    Text(Move(count))
                                        .Font(Monospace())
                                        .FontSize(ThemeMetric("FontSize.Small"))
                                        .TextColor(ThemeColor("Tree.Count"))})
        .Height(ThemeMetric("Tree.RowHeight"))
        .Shrink(0)
        .Gap(5)
        .AlignItems(UIAlign::Center)
        .Padding([depth] { return UIEdges(static_cast<float32>(depth) * ThemeMetric("Tree.Indent")() + 6, 0, 10, 0); })
        .Background(Animate<LinearColor>(
            [checks, hovered] {
                return ThemeColor(checks[0]() ? "Tree.Selected" : hovered.Get() ? "Tree.Hovered" : "Tree.Background")();
            },
            RowFade))
        .OnPointerDown([callbacks](UIPointerEvent &event) {
            if (event.Button == MouseButton::Left)
            {
                event.Handled = true;
                callbacks[0]();
            }
        });
}

/// A thin line across a dock.
UIElement Divider()
{
    return Box().Height(1).Shrink(0).Margin(UIEdges(0, 6)).Background(ThemeColor("Tree.Divider"));
}
} // namespace

bool AssetManagerDockType::CanDockIn(const EditingContext &context) const
{
    return AssetManagerContext::IsAssetManager(context);
}

// ---- Folders ----

UIElement AssetFoldersDock::Build(EditingContext &context) const
{
    AssetManagerContext &manager = AssetManagerOf(context);
    AssetBrowserState *state = &manager.GetState();
    AssetCatalogSubsystem *catalog = manager.GetCatalog();
    if (catalog == nullptr)
    {
        return Box();
    }
    const auto special = [state, catalog](StringView folder, StringView icon, StringView color, StringView label,
                                          bool counted) {
        const String path(folder);
        return TreeRow(
            1, [state, path] { return state->Folder.Get() == path; }, IconView(icon, ThemeColor(color), 13, 1.5f),
            String(label),
            [catalog, counted] { return counted ? Format("{0}", catalog->GetStarredCount()) : String(); },
            [state, path] { state->Folder.Set(path); }, false, [] { return false; }, [] {});
    };
    // The folders whose parents are all open, keyed by path and catalog version so they are built again on change.
    const auto visible = [state, catalog] {
        List<int32> rows;
        const List<CatalogFolder> &folders = catalog->GetFolders();
        const List<String> &collapsed = state->Collapsed.Get();
        for (usize index = 0; index < folders.Count(); ++index)
        {
            bool shown = true;
            for (int32 parent = folders[index].Parent; shown && parent >= 0;
                 parent = folders[static_cast<usize>(parent)].Parent)
            {
                shown = !collapsed.Contains(folders[static_cast<usize>(parent)].Path);
            }
            if (shown)
            {
                rows.Add(static_cast<int32>(index));
            }
        }
        return rows;
    };
    UIElement tree = For<int32>(
        visible,
        [catalog](const int32 &index) {
            return catalog->GetFolders()[static_cast<usize>(index)].Path.GetHash() ^
                   (static_cast<uint64>(catalog->GetVersion()) << 40);
        },
        [state, catalog](Signal<int32> item) {
            const CatalogFolder &folder = catalog->GetFolders()[static_cast<usize>(item.Peek())];
            const String path = folder.Path;
            const auto selected = [state, path] { return state->Folder.Get() == path; };
            const auto expanded = [state, path] { return !state->Collapsed.Get().Contains(path); };
            UIElement icon = Box({IconView("FolderOpen", ThemeColor("AssetManager.Folder"), 13, 1.5f).Visible(selected),
                                  IconView("Folder", ThemeColor("AssetManager.Folder"), 13, 1.5f).Visible([selected] {
                                      return !selected();
                                  })})
                                 .Shrink(0);
            return TreeRow(
                folder.Depth, selected, icon, folder.Name, Format("{0}", folder.Count),
                [state, path] { state->Folder.Set(path); }, !folder.Children.IsEmpty(), expanded,
                [state, path] {
                    state->Collapsed.Update([&path](List<String> &collapsed) {
                        if (!collapsed.Remove(path))
                        {
                            collapsed.Add(path);
                        }
                    });
                });
        });
    return ScrollView(
               Column(
                   {
                       GroupLabel("FAVORITES"),
                       special(AssetManagerContext::StarredFolder, "Star", "AssetManager.Star", "Starred", true),
                       special(AssetManagerContext::RecentFolder, "History", "Tree.Icon", "Recently Modified", false),
                       Divider(),
                       GroupLabel("CONTENT"),
                       tree,
                   })
                   .Padding(UIEdges(0, 4)))
        .Grow(1);
}

// ---- Browser ----

namespace
{
/// The folder path as links: Content > Environment > Props.
UIElement Breadcrumbs(AssetBrowserState *state, AssetCatalogSubsystem *catalog)
{
    struct Crumb
    {
        String Name;
        String Path;
    };
    const auto crumbs = [state, catalog] {
        List<Crumb> list;
        const String &folder = state->Folder.Get();
        if (folder == AssetManagerContext::StarredFolder || folder == AssetManagerContext::RecentFolder)
        {
            list.Add({folder == AssetManagerContext::StarredFolder ? String("Starred") : String("Recently Modified"),
                      folder});
            return list;
        }
        const List<CatalogFolder> &folders = catalog->GetFolders();
        for (int32 index = catalog->FindFolder(folder.AsView()); index >= 0;
             index = folders[static_cast<usize>(index)].Parent)
        {
            list.Insert(0, Crumb{folders[static_cast<usize>(index)].Name, folders[static_cast<usize>(index)].Path});
        }
        return list;
    };
    return For<Crumb>(
        crumbs, [](const Crumb &crumb) { return crumb.Path.GetHash(); },
        [state](Signal<Crumb> item) {
            const String path = item.Peek().Path;
            const auto last = [state, path] { return state->Folder.Get() == path; };
            UIElement link = Box();
            const Signal<bool> hovered = TrackHover(link);
            link = WithChildren(Move(link),
                                {Text(item.Peek().Name).FontSize(ThemeMetric("FontSize.Normal")).TextColor([last] {
                                    return ThemeColor(last() ? "Breadcrumb.Current" : "Breadcrumb.Text")();
                                })})
                       .Height(22)
                       .Justify(UIJustify::Center)
                       .Padding(UIEdges(5, 0))
                       .Radius([] {
                           const float32 radius = ThemeMetric("Radius.Medium")();
                           return UICornerRadii(radius, radius, radius, radius);
                       })
                       .Background(FadeIn([hovered] { return hovered.Get(); }, "Breadcrumb.Hovered", RowFade))
                       .OnClick([state, path](UIPointerEvent &event) {
                           event.Handled = true;
                           state->Folder.Set(path);
                       });
            return Row({link, IconView("ChevronRight", ThemeColor("Breadcrumb.Separator"), 10, 1.5f).Visible([last] {
                            return !last();
                        })})
                .AlignItems(UIAlign::Center)
                .Gap(2)
                .Shrink(0);
        });
}

/// "Sort Name ▾", opening a menu of the orders.
UIElement SortButton(AssetBrowserState *state)
{
    UIElement button = Row();
    UIElementNode *node = button.GetNode();
    const Signal<bool> hovered = TrackHover(button);
    return WithChildren(
               Move(button),
               {Text(String("Sort")).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor("Text.Secondary")),
                Text([state] {
                    constexpr StringView names[] = {"Name", "Type", "Modified"};
                    return String(names[Math::Clamp(state->Sort.Get(), 0, 2)]);
                })
                    .FontSize(ThemeMetric("FontSize.Normal"))
                    .TextColor(ThemeColor("Input.Text")),
                IconView("ChevronDown", ThemeColor("Input.Icon"), 10, 1.5f)})
        .Height(ThemeMetric("ControlHeight"))
        .Gap(8)
        .Padding(UIEdges(8, 0))
        .AlignItems(UIAlign::Center)
        .Shrink(0)
        .Radius([] {
            const float32 radius = ThemeMetric("Radius.Medium")();
            return UICornerRadii(radius, radius, radius, radius);
        })
        .Background([hovered] { return ThemeColor(hovered.Get() ? "Chip.HoveredBackground" : "Input.Background")(); })
        .Border(1, ThemeColor("Input.Border"))
        .OnClick([state, node](UIPointerEvent &event) {
            event.Handled = true;
            const Vector2 below = node->GetWindowPosition() + Vector2(0, node->GetBounds().Height + 2);
            List<UIMenuItem> items;
            constexpr StringView names[] = {"Name", "Type", "Modified"};
            for (int32 index = 0; index < 3; ++index)
            {
                items.Add({String(names[index]), state->Sort.Peek() == index ? "Star" : "", {}, [state, index] {
                               state->Sort.Set(index);
                           }});
            }
            ShowContextMenu(node, below, Move(items));
        });
}

/// Press handling shared by tiles and list rows: select, open on a double click, actions on a right click.
void HandleAssetPress(UIElement &element, AssetManagerContext *manager, Guid uuid)
{
    UIElementNode *node = element.GetNode();
    element.OnPointerDown([manager, uuid, node](UIPointerEvent &event) {
        AssetBrowserState &state = manager->GetState();
        if (event.Button == MouseButton::Right)
        {
            event.Handled = true;
            manager->ShowAssetMenu(node, event.Position, uuid);
            return;
        }
        if (event.Button != MouseButton::Left)
        {
            return;
        }
        event.Handled = true;
        state.Selected.Set(uuid);
        if (event.Clicks == 2)
        {
            manager->Open(uuid);
        }
    });
}

UIElement Tile(AssetManagerContext *manager, AssetCatalogSubsystem *catalog, const CatalogAsset &asset)
{
    AssetBrowserState *state = &manager->GetState();
    const Guid uuid = asset.Uuid;
    const AssetTypeStyle &style = AssetCatalogSubsystem::GetStyle(asset.Type);
    const String color = style.Color;
    const auto selected = [state, uuid] { return state->Selected.Get() == uuid; };
    UIElement star = IconView("Star", ThemeColor("AssetManager.Star"), 12, 1.5f).Visible([catalog, uuid] {
        return catalog->IsStarred(uuid);
    });
    UIElement thumbnail = Column({
                                     Row({Box().Grow(1), star}).Height(12).Shrink(0).Margin(UIEdges(5, 5, 5, 0)),
                                     Box({IconView(style.Icon.AsView(), ThemeColor(color.AsView()), 40, 2)})
                                         .Grow(1)
                                         .Justify(UIJustify::Center)
                                         .AlignItems(UIAlign::Center),
                                     Box().Height(2).Shrink(0).Background(ThemeColor(color.AsView())),
                                 })
                              .Height([state] { return UILength(state->TileSize.Get() * 0.85f); })
                              .Shrink(0)
                              .Background(ThemeColor("Tile.Thumbnail"));
    UIElement label =
        Column({
                   Box({Text(asset.Name).FontSize(ThemeMetric("FontSize.Normal")).TextColor([selected] {
                       return ThemeColor(selected() ? "Tile.SelectedText" : "Tile.Text")();
                   })}).Clip(),
                   Text(style.Label).FontSize(ThemeMetric("FontSize.Small")).TextColor(ThemeColor("Tile.Type")),
               })
            .Gap(1)
            .Padding(UIEdges(6, 5, 6, 6));
    UIElement tile = Column({thumbnail, label});
    const Signal<bool> hovered = TrackHover(tile);
    tile.Clip()
        .Radius([] {
            const float32 radius = ThemeMetric("Radius.Large")();
            return UICornerRadii(radius, radius, radius, radius);
        })
        .Background(Animate<LinearColor>(
            [selected, hovered] {
                return ThemeColor(selected()      ? "Tile.SelectedBackground"
                                  : hovered.Get() ? "Tile.Hovered"
                                                  : "Tile.Background")();
            },
            RowFade))
        .Border(1, [selected] { return ThemeColor(selected() ? "Tile.SelectedBorder" : "Tile.Border")(); });
    HandleAssetPress(tile, manager, uuid);
    return tile;
}

/// The widths of the list's columns: name and path share what the fixed ones leave.
UIElement ListCells(UIElement name, UIElement type, UIElement path, UIElement size, UIElement modified)
{
    return Row({name.Basis(UILength(0)).Grow(1.4f).Clip(), type.Width(90).Shrink(0).Clip(),
                path.Basis(UILength(0)).Grow(1).Clip(), size.Width(70).Shrink(0), modified.Width(90).Shrink(0)})
        .AlignItems(UIAlign::Center)
        .Gap(6)
        .Padding(UIEdges(10, 0, 8, 0));
}

UIElement ListRow(AssetManagerContext *manager, const CatalogAsset &asset, usize index)
{
    AssetBrowserState *state = &manager->GetState();
    const Guid uuid = asset.Uuid;
    const AssetTypeStyle &style = AssetCatalogSubsystem::GetStyle(asset.Type);
    const auto selected = [state, uuid] { return state->Selected.Get() == uuid; };
    const auto cell = [](String text, StringView color, bool mono = false) {
        UIElement element = Text(Move(text)).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor(color));
        if (mono)
        {
            element.Font(Monospace());
        }
        return Box({element});
    };
    UIElement row = ListCells(
        Row({TypeIcon(asset.Type, 13), Text(asset.Name).FontSize(ThemeMetric("FontSize.Normal")).TextColor([selected] {
                 return ThemeColor(selected() ? "Tile.SelectedText" : "Tile.Text")();
             })})
            .Gap(6)
            .AlignItems(UIAlign::Center),
        cell(style.Label, "List.Secondary"), cell(DisplayPath(asset.Folder.AsView()), "List.Path"),
        cell(FormatSize(asset.Size), "List.Secondary", true).Justify(UIJustify::End).Direction(UIDirection::Row),
        cell(FormatDate(asset.Modified), "List.Path", true));
    const bool odd = index % 2 == 1;
    const Signal<bool> hovered = TrackHover(row);
    row.Height(ThemeMetric("List.RowHeight")).Shrink(0).Background([selected, hovered, odd] {
        return ThemeColor(selected()      ? "List.Selected"
                          : hovered.Get() ? "List.Hovered"
                          : odd           ? "List.Alternate"
                                          : "List.Background")();
    });
    HandleAssetPress(row, manager, uuid);
    return row;
}
} // namespace

UIElement AssetBrowserDock::Build(EditingContext &context) const
{
    AssetManagerContext *manager = &AssetManagerOf(context);
    AssetBrowserState *state = &manager->GetState();
    AssetCatalogSubsystem *catalog = manager->GetCatalog();
    if (catalog == nullptr)
    {
        return Box();
    }

    UIElement bar =
        Row({
                Row({Breadcrumbs(state, catalog)}).Grow(1).AlignItems(UIAlign::Center).Clip(),
                TextField(state->Search, "Search assets, tags…", "Search").Width(280).Shrink(1).MinWidth(120),
                SortButton(state),
            })
            .Gap(10)
            .AlignItems(UIAlign::Center)
            .Padding(UIEdges(8, 6))
            .Shrink(0)
            .Background(ThemeColor("Browser.Bar"));

    // A chip per type the catalog has, by label.
    const auto types = [catalog] {
        List<String> found;
        for (const CatalogAsset &asset : catalog->GetAssets())
        {
            if (!found.Contains(asset.Type))
            {
                found.Add(asset.Type);
            }
        }
        found.Sort([](const String &a, const String &b) {
            return AssetCatalogSubsystem::GetStyle(a.AsView()).Label.AsView() <
                   AssetCatalogSubsystem::GetStyle(b.AsView()).Label.AsView();
        });
        return found;
    };
    UIElement chips = Row({For<String>(
                               types, [](const String &type) { return type.GetHash(); },
                               [state](Signal<String> item) {
                                   const String type = item.Peek();
                                   const AssetTypeStyle &style = AssetCatalogSubsystem::GetStyle(type.AsView());
                                   return Chip(
                                       style.Icon.AsView(), ThemeColor(style.Color.AsView()), style.Label,
                                       [state, type] { return state->Types.Get().Contains(type); },
                                       [state, type] {
                                           state->Types.Update([&type](List<String> &active) {
                                               if (!active.Remove(type))
                                               {
                                                   active.Add(type);
                                               }
                                           });
                                       });
                               }),
                           Show([state] { return !state->Types.Get().IsEmpty(); },
                                [state] {
                                    return Box({Text(String("Clear filters"))
                                                    .FontSize(ThemeMetric("FontSize.Normal"))
                                                    .TextColor(ThemeColor("Text.Link"))})
                                        .Padding(UIEdges(6, 0))
                                        .OnClick([state](UIPointerEvent &event) {
                                            event.Handled = true;
                                            state->Types.Set(List<String>());
                                        });
                                })})
                          .Gap(4)
                          .AlignItems(UIAlign::Center)
                          .Padding(UIEdges(8, 6))
                          .Shrink(0)
                          .Clip();

    const auto key = [catalog](const Guid &uuid) {
        return uuid.GetHash() ^ (static_cast<uint64>(catalog->GetVersion()) << 48);
    };
    const auto visible = [manager] { return manager->GetVisible(); };
    UIElement tiles = Grid({For<Guid>(visible, key,
                                      [manager, catalog](Signal<Guid> item) {
                                          const CatalogAsset *asset = catalog->Find(item.Peek());
                                          return asset != nullptr ? Tile(manager, catalog, *asset) : Box();
                                      })})
                          .AutoFillColumns(state->TileSize)
                          .Gap(ThemeMetric("Tile.Gap"))
                          .Padding(ThemeMetric("Tile.Gap"));
    const auto header = [](StringView text) {
        return Box(
            {Text(String(text)).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor("List.HeaderText"))});
    };
    UIElement list = Column({
        ListCells(header("Name"), header("Type"), header("Path"),
                  header("Size").Justify(UIJustify::End).Direction(UIDirection::Row), header("Modified"))
            .Height(ThemeMetric("List.RowHeight"))
            .Shrink(0)
            .Background(ThemeColor("List.Header")),
        ScrollView(Column({For<Guid>(visible, key,
                                     [manager, catalog](Signal<Guid> item) {
                                         const CatalogAsset *asset = catalog->Find(item.Peek());
                                         const usize index = manager->GetVisible().IndexOf(item.Peek());
                                         return asset != nullptr ? ListRow(manager, *asset, index) : Box();
                                     })}))
            .Grow(1)
            .Basis(UILength(0)),
    });
    UIElement empty = Box({Text([catalog] {
                               return String(catalog->IsConnected() ? "No assets match the current search and filters."
                                                                    : "The asset indexer is not running.");
                           })
                               .FontSize(ThemeMetric("FontSize.Medium"))
                               .TextColor(ThemeColor("Details.Empty"))})
                          .Padding(UIEdges(20, 60))
                          .Justify(UIJustify::Center)
                          .AlignItems(UIAlign::Center)
                          .Visible([manager] { return manager->GetVisible().IsEmpty(); });
    UIElement content = Column({empty, ScrollView(tiles).Grow(1).Basis(UILength(0)).Visible([state, manager] {
                                    return state->View.Get() == 0 && !manager->GetVisible().IsEmpty();
                                }),
                                list.Grow(1).Basis(UILength(0)).Visible([state, manager] {
                                    return state->View.Get() == 1 && !manager->GetVisible().IsEmpty();
                                })})
                            .Grow(1)
                            .Basis(UILength(0))
                            .Background(ThemeColor("Browser.Background"));

    UIElement footer =
        Row({
                Text([manager, state] {
                    const usize count = manager->GetVisible().Count();
                    const bool selected = manager->GetVisible().Contains(state->Selected.Get());
                    return Format("{0} item{1}{2}", count, count == 1 ? "" : "s", selected ? " · 1 selected" : "");
                })
                    .FontSize(ThemeMetric("FontSize.Normal"))
                    .TextColor(ThemeColor("Text.Secondary"))
                    .Grow(1),
                Slider(state->TileSize, 72, 180).Width(90).Opacity([state] {
                    return state->View.Get() == 0 ? 1.0f : 0.35f;
                }),
                SegmentedIcons({"LayoutGrid", "List"}, state->View),
            })
            .Height(26)
            .Shrink(0)
            .Gap(10)
            .AlignItems(UIAlign::Center)
            .Padding(UIEdges(8, 0))
            .Background(ThemeColor("Browser.Bar"));

    return Column({bar, chips, Box().Height(1).Shrink(0).Background(ThemeColor("Browser.Divider")), content, footer})
        .Grow(1);
}

// ---- Details ----

namespace
{
UIElement Properties(const CatalogAsset &asset)
{
    UIElement grid = Grid().Columns({UIGridTrack::Points(90), UIGridTrack::Fraction(1)}).Gap(6);
    const auto add = [&grid](StringView key, String value, bool mono) {
        UIElement text =
            Text(Move(value)).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor("Details.Value"));
        if (mono)
        {
            text.Font(Monospace());
        }
        grid.GetNode()->AppendChild(
            Text(String(key)).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor("Details.Key")).GetNode());
        grid.GetNode()->AppendChild(Box({text}).Clip().GetNode());
    };
    add("Type", AssetCatalogSubsystem::GetStyle(asset.Type).Label, false);
    add("Source Type", asset.Type, false);
    add("Path", DisplayPath(asset.File.AsView()), true);
    add("Disk Size", FormatSize(asset.Size), false);
    add("Modified", FormatDate(asset.Modified), false);
    add("GUID", Format("{0}", asset.Uuid), true);
    return grid.Padding(UIEdges(22, 6, 10, 10));
}

UIElement Tags(const CatalogAsset &asset, AssetBrowserState *state)
{
    UIElement row = Row().Gap(4).AlignItems(UIAlign::Center).Padding(UIEdges(22, 8, 10, 10)).Clip();
    for (const String &tag : asset.Tags)
    {
        UIElement pill = Box();
        const Signal<bool> hovered = TrackHover(pill);
        pill = WithChildren(Move(pill),
                            {Text(tag).FontSize(ThemeMetric("FontSize.Small")).TextColor(ThemeColor("Tag.Text"))})
                   .Height(20)
                   .Justify(UIJustify::Center)
                   .Padding(UIEdges(7, 0))
                   .Shrink(0)
                   .Radius([] {
                       const float32 radius = ThemeMetric("Radius.Medium")();
                       return UICornerRadii(radius, radius, radius, radius);
                   })
                   .Background(ThemeColor("Tag.Background"))
                   .Border(1, [hovered] { return ThemeColor(hovered.Get() ? "Tag.HoveredBorder" : "Tag.Border")(); })
                   .OnClick([state, tag](UIPointerEvent &event) {
                       event.Handled = true;
                       state->Search.Set(tag);
                   });
        row.GetNode()->AppendChild(pill.GetNode());
    }
    if (asset.Tags.IsEmpty())
    {
        row.GetNode()->AppendChild(Text(String("No tags."))
                                       .FontSize(ThemeMetric("FontSize.Normal"))
                                       .TextColor(ThemeColor("Details.Empty"))
                                       .GetNode());
    }
    row.GetNode()->AppendChild(IconButton("Plus", [] {}, false, 20).GetNode());
    return row;
}

UIElement ReferencedBy(const CatalogAsset &asset, AssetManagerContext *manager, AssetCatalogSubsystem *catalog)
{
    List<CatalogAsset> referencers;
    catalog->GetReferencers(asset.Uuid, referencers);
    UIElement column = Column().Padding(UIEdges(0, 4, 0, 10));
    for (const CatalogAsset &referencer : referencers)
    {
        UIElement row = Row();
        const Signal<bool> hovered = TrackHover(row);
        const Guid uuid = referencer.Uuid;
        row = WithChildren(Move(row), {TypeIcon(referencer.Type, 12),
                                       Box({Text(referencer.Name)
                                                .FontSize(ThemeMetric("FontSize.Normal"))
                                                .TextColor(ThemeColor("Details.Value"))})
                                           .Grow(1)
                                           .Clip(),
                                       Text(AssetCatalogSubsystem::GetStyle(referencer.Type).Label)
                                           .FontSize(ThemeMetric("FontSize.Small"))
                                           .TextColor(ThemeColor("Tile.Type"))})
                  .Height(20)
                  .Gap(6)
                  .AlignItems(UIAlign::Center)
                  .Padding(UIEdges(22, 0, 10, 0))
                  .Background(FadeIn([hovered] { return hovered.Get(); }, "Tree.Hovered", RowFade))
                  .OnClick([manager, uuid](UIPointerEvent &event) {
                      event.Handled = true;
                      manager->ShowInFolder(uuid);
                  });
        column.GetNode()->AppendChild(row.GetNode());
    }
    if (referencers.IsEmpty())
    {
        column.GetNode()->AppendChild(Box({Text(String("Not referenced by other assets."))
                                               .FontSize(ThemeMetric("FontSize.Normal"))
                                               .TextColor(ThemeColor("Details.Empty"))})
                                          .Padding(UIEdges(22, 2))
                                          .GetNode());
    }
    return column;
}

UIElement AssetDetails(const CatalogAsset &asset, AssetManagerContext *manager, AssetCatalogSubsystem *catalog)
{
    const Guid uuid = asset.Uuid;
    const AssetTypeStyle &style = AssetCatalogSubsystem::GetStyle(asset.Type);
    // Until thumbnails and preview viewports exist, the preview is the type's icon.
    const Signal<int32> shading = CreateSignal<int32>(0);
    UIElement preview =
        Box({Box({TypeIcon(asset.Type, 64, 2.5f)}).Grow(1).Justify(UIJustify::Center).AlignItems(UIAlign::Center),
             SegmentedLabels({"Lit", "Unlit", "Wire"}, shading).Absolute(Vector2(6, 6))})
            .Height(180)
            .Shrink(0)
            .Margin(UIEdges(8))
            .Radius([] {
                const float32 radius = ThemeMetric("Radius.Large")();
                return UICornerRadii(radius, radius, radius, radius);
            })
            .Background(ThemeColor("Details.Preview"))
            .Border(1, ThemeColor("Details.PreviewBorder"));
    UIElement star = Box();
    const Signal<bool> starHovered = TrackHover(star);
    star = WithChildren(Move(star),
                        {IconView(
                            "Star",
                            [catalog, uuid] {
                                return ThemeColor(catalog->IsStarred(uuid) ? "AssetManager.Star" : "Details.Key")();
                            },
                            15, 1.5f)})
               .Width(24)
               .Height(24)
               .Shrink(0)
               .Justify(UIJustify::Center)
               .AlignItems(UIAlign::Center)
               .Background(FadeIn([starHovered] { return starHovered.Get(); }, "Tree.Hovered", RowFade))
               .OnClick([catalog, uuid](UIPointerEvent &event) {
                   event.Handled = true;
                   catalog->SetStarred(uuid, !catalog->IsStarred(uuid));
               });
    UIElement title =
        Row({TypeIcon(asset.Type, 16),
             Column(
                 {Box({Text(asset.Name).FontSize(ThemeMetric("FontSize.Medium")).TextColor(ThemeColor("Text.Strong"))})
                      .Clip(),
                  Text(style.Label).FontSize(ThemeMetric("FontSize.Small")).TextColor(ThemeColor("Details.Key"))})
                 .Grow(1)
                 .Basis(UILength(0)),
             star})
            .Gap(8)
            .AlignItems(UIAlign::Center)
            .Padding(UIEdges(10, 2, 10, 8))
            .Shrink(0);
    const String uuidText = Format("{0}", uuid);
    UIElement actions =
        Row({Button(
                 String("Open"), [manager, uuid] { manager->Open(uuid); }, UIButtonKind::Primary)
                 .Grow(1)
                 .Basis(UILength(0)),
             Button(String("Show in Folder"), [manager, uuid] { manager->ShowInFolder(uuid); })
                 .Grow(1)
                 .Basis(UILength(0)),
             IconButton("Copy", [uuidText] { (void)PlatformApplication::SetClipboardText(uuidText.CStr()); })})
            .Gap(6)
            .Padding(UIEdges(10, 0, 10, 10))
            .Shrink(0);
    return ScrollView(Column({preview, title, actions, Section(String("Properties"), Properties(asset)),
                              Section(String("Tags"), Tags(asset, &manager->GetState())),
                              Section(String("Referenced By"), ReferencedBy(asset, manager, catalog))}));
}
} // namespace

UIElement AssetDetailsDock::Build(EditingContext &context) const
{
    AssetManagerContext *manager = &AssetManagerOf(context);
    AssetBrowserState *state = &manager->GetState();
    AssetCatalogSubsystem *catalog = manager->GetCatalog();
    if (catalog == nullptr)
    {
        return Box();
    }
    // Built again for each selection and each read of the catalog: a list of at most one, keyed by both.
    const auto shown = [state, catalog] {
        List<Guid> shown;
        if (catalog->Find(state->Selected.Get()) != nullptr)
        {
            shown.Add(state->Selected.Get());
        }
        return shown;
    };
    UIElement details = For<Guid>(
        shown,
        [catalog](const Guid &uuid) { return uuid.GetHash() ^ (static_cast<uint64>(catalog->GetVersion()) << 48); },
        [manager, catalog](Signal<Guid> item) {
            const CatalogAsset *asset = catalog->Find(item.Peek());
            return asset != nullptr ? AssetDetails(*asset, manager, catalog).Grow(1) : Box();
        });
    UIElement empty = Box({Text(String("Select an asset to preview it."))
                               .FontSize(ThemeMetric("FontSize.Normal"))
                               .TextColor(ThemeColor("Details.Empty"))})
                          .Padding(UIEdges(20, 40))
                          .Justify(UIJustify::Center)
                          .AlignItems(UIAlign::Center)
                          .Visible([shown] { return shown().IsEmpty(); });
    return Column({empty, details}).Grow(1);
}
} // namespace Marlin
