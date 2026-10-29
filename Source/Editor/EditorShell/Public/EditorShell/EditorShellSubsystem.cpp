#include "EditorShell/EditorShellSubsystem.h"

#include "Application/ApplicationSubsystem.h"
#include "Application/PlatformWindow.h"
#include "Containers/HashMap.h"
#include "EditorShell/EditingContextSubsystem.h"
#include "EditorShell/EditorMenuSubsystem.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/EditorUISubsystem.h"
#include "EditorUI/UIAnimation.h"
#include "EditorUI/UIWidgets.h"
#include "Engine/Engine.h"
#include "Math/MathUtility.h"
#include "String/Format.h"
#include "UIPainter/UIPainter.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
namespace
{
/// How far the pointer moves with a tab held before the tab starts dragging, in painter units.
constexpr float32 DragThreshold = 4;
/// How far one wheel notch scrolls the tab strip, in painter units.
constexpr float32 WheelStep = 48;
/// Tabs sliding to new places in the row.
constexpr UITransition TabSlide{0.16f, &Easing::EaseOutCubic};
/// Hover and press colors fading in and out.
constexpr UITransition HoverFade{0.08f, &Easing::EaseOutCubic};
/// Menus popping open.
constexpr UITransition MenuPop{0.14f, &Easing::EaseOutBack};

/// The window rectangle of *node* as laid out, without its own translation.
UIRect LayoutRect(const UIElementNode *node)
{
    const Vector2 position = node->GetWindowPosition() - node->Style.Translate;
    return {position.X, position.Y, node->GetBounds().Width, node->GetBounds().Height};
}
} // namespace

/// The tab strip's scroll and the tab being dragged. Painting applies both, so they always match the latest layout.
struct EditorShellSubsystem::TabStrip
{
    UIElementNode *Viewport = nullptr;
    UIElementNode *Content = nullptr;
    HashMap<EditingContext *, UIElementNode *> Tabs;
    float32 Scroll = 0;
    /// Scrolls the active tab into view when it changes.
    EditingContext *Revealed = nullptr;

    EditingContext *Pressed = nullptr;
    bool Dragging = false;
    float32 PressX = 0;
    /// The pointer's distance from the dragged tab's left edge.
    float32 GrabX = 0;
    float32 PointerX = 0;

    /// A tab sliding from where it was drawn to where layout put it (a FLIP animation): when its place in the row
    /// changes, it starts drawn at the old place and its offset eases to zero.
    struct Motion
    {
        bool Placed = false;
        float32 LayoutX = 0;
        float32 From = 0;
        float64 Start = 0;
    };
    HashMap<EditingContext *, Motion> Motions;

    /// The offset of *motion* now.
    static float32 Offset(const Motion &motion, float64 now)
    {
        const float64 progress = (now - motion.Start) / TabSlide.Duration;
        return progress >= 1 ? 0.0f : motion.From * (1 - TabSlide.Easing(static_cast<float32>(Max(progress, 0.0))));
    }
};

/// The open menu and the menu bar's labels, which the overlay tests the pointer against.
struct EditorShellSubsystem::MenuState
{
    Signal<int32> Open;
    Signal<Vector2> Anchor;
    List<UIElementNode *> Labels;
};

EditorShellSubsystem::EditorShellSubsystem() = default;
EditorShellSubsystem::~EditorShellSubsystem() = default;

void EditorShellSubsystem::Initialize(SubsystemContext &context)
{
    _application = Engine::GetSubsystem<ApplicationSubsystem>();
    _text = Engine::GetSubsystem<UITextSubsystem>();
    _ui = context.Collection->Get<EditorUISubsystem>();
    _contexts = context.Collection->Get<EditingContextSubsystem>();
    _menus = context.Collection->Get<EditorMenuSubsystem>();
    if (_ui == nullptr || _ui->GetMainWindow() == nullptr || _contexts == nullptr || _menus == nullptr)
    {
        return;
    }
    _owner = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(_owner, [this] {
        const WindowHandle window = _application != nullptr ? _application->GetMainWindow() : WindowHandle();
        _maximized = CreateSignal(window.IsValid() && PlatformWindow::IsMaximized(window));
        _menu = new MenuState{CreateSignal<int32>(-1), CreateSignal(Vector2()), {}};
        Reactive::OnCleanup([this] {
            delete _menu;
            _menu = nullptr;
        });
    });

    EditorMenuItem exit;
    exit.Label = "Exit";
    exit.Shortcut = "Alt+F4";
    exit.Order = 1000;
    exit.Execute = [this] {
        if (_application != nullptr)
        {
            PlatformWindow::RequestClose(_application->GetMainWindow());
        }
    };
    _menus->AddItem("File", Move(exit));

    _ui->GetMainWindow()->SetContent([this] { return BuildWindow(); });
}

void EditorShellSubsystem::Deinitialize()
{
    if (_ui != nullptr && _ui->GetMainWindow() != nullptr)
    {
        _ui->GetMainWindow()->SetContent([] { return Box(); });
    }
    if (_owner != nullptr)
    {
        Reactive::DisposeOwner(_owner);
        _owner = nullptr;
    }
}

void EditorShellSubsystem::BeginFrame(const FrameTime & /*time*/)
{
    if (_application == nullptr || _owner == nullptr)
    {
        return;
    }
    const WindowHandle window = _application->GetMainWindow();
    for (const ApplicationEvent &event : _application->GetEvents())
    {
        if (event.Window == window && event.Type == ApplicationEventType::WindowResized)
        {
            _maximized.Set(PlatformWindow::IsMaximized(window));
        }
    }
}

UIElement EditorShellSubsystem::BuildWindow()
{
    return Box({
                   BuildTitleBar(),
                   BuildMenuBar(),
                   BuildContent(),
                   BuildStatusBar(),
                   BuildMenuOverlay(),
               })
        .Grow(1)
        .Background(ThemeColor("Window.Background"));
}

UIElement EditorShellSubsystem::BuildTitleBar()
{
    // Tabs sit on the bar's bottom edge, so the active one runs into the menu bar below it.
    return Row({BuildTabStrip(), BuildWindowButtons()})
        .Height(ThemeMetric("TitleBar.Height"))
        .Shrink(0)
        .AlignItems(UIAlign::End)
        .Padding([] { return UIEdges(ThemeMetric("TitleBar.PaddingLeft")(), 0, 0, 0); })
        .Background(ThemeColor("TitleBar.Background"))
        .WindowCaption();
}

UIElement EditorShellSubsystem::BuildTabStrip()
{
    auto *strip = new TabStrip;
    Reactive::OnCleanup([strip] { delete strip; });
    EditingContextSubsystem *contexts = _contexts;

    UIElement tabs =
        Row({For<EditingContext *>(
                [contexts] { return contexts->GetContexts(); },
                [](EditingContext *const &context) { return reinterpret_cast<uint64>(context); },
                [this, strip](Signal<EditingContext *> context) { return BuildTab(context.Peek(), strip); })})
            .Gap(ThemeMetric("TitleBar.Gap"))
            .AlignItems(UIAlign::End)
            .Shrink(0);
    strip->Content = tabs.GetNode();

    UIElement viewport = Row({tabs})
                             .Grow(1)
                             .AlignSelf(UIAlign::Stretch)
                             .AlignItems(UIAlign::End)
                             .Clip()
                             .OnWheel([strip](UIPointerEvent &event) {
                                 strip->Scroll -= (event.Wheel.Y != 0 ? event.Wheel.Y : -event.Wheel.X) * WheelStep;
                                 event.Handled = true;
                             })
                             .OnPaint([strip, contexts](UIPainter &, const UIRect &rect) {
                                 // Before the tabs paint: clamp the scroll to this layout, reveal a newly active tab,
                                 // place the drag.
                                 const float32 overflow = Max(strip->Content->GetBounds().Width - rect.Width, 0.0f);
                                 EditingContext *active = contexts->GetActive();
                                 UIElementNode *const *activeTab = strip->Tabs.Find(active);
                                 if (active != strip->Revealed && activeTab != nullptr)
                                 {
                                     strip->Revealed = active;
                                     const UIRect &bounds = (*activeTab)->GetBounds();
                                     strip->Scroll = Min(strip->Scroll, bounds.X);
                                     strip->Scroll = Max(strip->Scroll, bounds.X + bounds.Width - rect.Width);
                                 }
                                 strip->Scroll = Math::Clamp(strip->Scroll, 0.0f, overflow);
                                 strip->Content->Style.Translate = Vector2(-strip->Scroll, 0);
                                 const float64 now = UIAnimation::PeekNow();
                                 for (const auto &entry : strip->Tabs)
                                 {
                                     // Relative to the row, so scrolling doesn't count as moving.
                                     const float32 x = entry.Value->GetBounds().X;
                                     TabStrip::Motion &motion = strip->Motions.FindOrAdd(entry.Key);
                                     if (motion.Placed && x != motion.LayoutX)
                                     {
                                         motion.From = motion.LayoutX + TabStrip::Offset(motion, now) - x;
                                         motion.Start = now;
                                     }
                                     motion.Placed = true;
                                     motion.LayoutX = x;
                                     entry.Value->Style.Translate = Vector2(TabStrip::Offset(motion, now), 0);
                                 }
                                 if (strip->Dragging)
                                 {
                                     if (UIElementNode *const *dragged = strip->Tabs.Find(strip->Pressed))
                                     {
                                         const float32 left =
                                             Math::Clamp(strip->PointerX - strip->GrabX, rect.X,
                                                         rect.X + rect.Width - (*dragged)->GetBounds().Width);
                                         (*dragged)->Style.Translate = Vector2(left - LayoutRect(*dragged).X, 0);
                                     }
                                 }
                             });
    strip->Viewport = viewport.GetNode();
    return viewport;
}

UIElement EditorShellSubsystem::BuildTab(EditingContext *context, TabStrip *strip)
{
    EditingContextSubsystem *contexts = _contexts;
    const bool pinned = context->IsPinned();
    const auto active = [contexts, context] { return contexts->GetActive() == context; };

    UIElement close;
    if (!pinned)
    {
        close = Box();
        const Signal<bool> closeHovered = TrackHover(close);
        close =
            WithChildren(
                Move(close),
                {IconView(
                    "X", [closeHovered] { return ThemeColor(closeHovered.Get() ? "Tab.CloseHovered" : "Tab.Close")(); },
                    ThemeMetric("Tab.CloseSize")(), 1.5f)})
                .Shrink(0)
                .OnPointerDown([](UIPointerEvent &event) { event.Handled = true; })
                .OnClick([contexts, context](UIPointerEvent &event) {
                    event.Handled = true;
                    (void)contexts->Close(context);
                });
    }

    UIElement tab = Row({
                            IconView(context->Icon.AsView(), ThemeColor(context->IconColor.AsView()),
                                     ThemeMetric("Tab.IconSize")()),
                            Text(context->Title).Grow(1).FontSize(ThemeMetric("FontSize.Medium")).TextColor([active] {
                                return active() ? ThemeColor("Tab.ActiveText")() : ThemeColor("Tab.Text")();
                            }),
                            close,
                        })
                        .Height(ThemeMetric("Tab.Height"))
                        .MinWidth(ThemeMetric("Tab.MinWidth"))
                        .Shrink(0)
                        .Gap(ThemeMetric("Tab.Gap"))
                        .AlignItems(UIAlign::Center)
                        // The top inset is the indicator's line, drawn over it on the active tab.
                        .Padding([] {
                            const float32 x = ThemeMetric("Tab.PaddingX")();
                            return UIEdges(x, ThemeMetric("Tab.IndicatorHeight")(), x, 0);
                        })
                        .Radius([] {
                            const float32 radius = ThemeMetric("Tab.Radius")();
                            return UICornerRadii(radius, radius, 0, 0);
                        });
    const Signal<bool> hovered = TrackHover(tab);
    tab.Background(Animate<LinearColor>(
        [active, hovered] {
            return ThemeColor(active() ? "Tab.Active" : hovered.Get() ? "Tab.Hovered" : "Tab.Background")();
        },
        HoverFade));
    tab.OnPaint([active](UIPainter &painter, const UIRect &rect) {
        if (active())
        {
            // The line follows the tab's rounded top corners: clipped to the tab's shape rather than rounded itself,
            // since the radius is larger than the line is thick.
            const float32 radius = ThemeMetric("Tab.Radius")();
            painter.PushClip(rect, UICornerRadii(radius, radius, 0, 0));
            painter.FillRect({rect.X, rect.Y, rect.Width, ThemeMetric("Tab.IndicatorHeight")()},
                             ThemeColor("Tab.Indicator")());
            painter.PopClip();
        }
    });
    tab.OnPointerDown([contexts, context, strip, pinned](UIPointerEvent &event) {
        if (event.Button == MouseButton::Middle)
        {
            (void)contexts->Close(context);
            return;
        }
        if (event.Button != MouseButton::Left)
        {
            return;
        }
        contexts->Activate(context);
        strip->Pressed = pinned ? nullptr : context;
        strip->Dragging = false;
        strip->PressX = event.Position.X;
        strip->PointerX = event.Position.X;
        strip->GrabX = event.LocalPosition.X;
    });
    tab.OnPointerMove([contexts, context, strip](UIPointerEvent &event) {
        if (strip->Pressed != context)
        {
            return;
        }
        strip->PointerX = event.Position.X;
        if (!strip->Dragging && Math::Abs(event.Position.X - strip->PressX) < DragThreshold)
        {
            return;
        }
        strip->Dragging = true;
        // Swap with a neighbour once the dragged tab's centre passes the neighbour's centre.
        UIElementNode *const *self = strip->Tabs.Find(context);
        if (self == nullptr)
        {
            return;
        }
        const float32 centre = strip->PointerX - strip->GrabX + (*self)->GetBounds().Width * 0.5f;
        const List<EditingContext *> &order = contexts->GetContexts();
        const usize index = order.IndexOf(context);
        usize target = index;
        while (target + 1 < order.Count())
        {
            UIElementNode *const *next = strip->Tabs.Find(order[target + 1]);
            if (next == nullptr || centre < LayoutRect(*next).X + LayoutRect(*next).Width * 0.5f)
            {
                break;
            }
            ++target;
        }
        while (target == index && target > 0 && !order[target - 1]->IsPinned())
        {
            UIElementNode *const *previous = strip->Tabs.Find(order[target - 1]);
            if (previous == nullptr || centre > LayoutRect(*previous).X + LayoutRect(*previous).Width * 0.5f)
            {
                break;
            }
            --target;
        }
        while (target < index && target > 0 && !order[target - 1]->IsPinned())
        {
            UIElementNode *const *previous = strip->Tabs.Find(order[target - 1]);
            if (previous == nullptr || centre > LayoutRect(*previous).X + LayoutRect(*previous).Width * 0.5f)
            {
                break;
            }
            --target;
        }
        if (target != index)
        {
            contexts->Reorder(context, target);
        }
    });
    tab.OnPointerUp([strip, context](UIPointerEvent &) {
        if (strip->Pressed != context)
        {
            return;
        }
        if (strip->Dragging)
        {
            // Glide from where it was dropped into its place.
            UIElementNode *const *node = strip->Tabs.Find(context);
            TabStrip::Motion *motion = strip->Motions.Find(context);
            if (node != nullptr && motion != nullptr)
            {
                motion->From = (*node)->Style.Translate.X;
                motion->Start = UIAnimation::PeekNow();
                motion->LayoutX = (*node)->GetBounds().X;
            }
        }
        strip->Pressed = nullptr;
        strip->Dragging = false;
    });

    strip->Tabs.Set(context, tab.GetNode());
    Reactive::OnCleanup([strip, context] {
        strip->Tabs.Remove(context);
        strip->Motions.Remove(context);
        if (strip->Pressed == context)
        {
            strip->Pressed = nullptr;
            strip->Dragging = false;
        }
    });
    return tab;
}

UIElement EditorShellSubsystem::BuildWindowButtons()
{
    // Full-height boxes that fill on hover, like the menu bar's labels; *prefix* names the button's tokens.
    const auto button = [](StringView icon, StringView prefix, Function<void()> action) {
        UIElement element = Box();
        const Signal<bool> hovered = TrackHover(element);
        const String hoveredGlyph = Format("{0}.HoveredGlyph", prefix);
        const String hoveredBackground = Format("{0}.Hovered", prefix);
        return WithChildren(Move(element), {IconView(
                                               icon,
                                               [hovered, hoveredGlyph] {
                                                   return ThemeColor(hovered.Get()
                                                                         ? hoveredGlyph.AsView()
                                                                         : StringView("WindowButton.Glyph"))();
                                               },
                                               ThemeMetric("WindowButton.IconSize")(), 1)})
            .Width(ThemeMetric("WindowButton.Width"))
            .Justify(UIJustify::Center)
            .AlignItems(UIAlign::Center)
            .Background(FadeIn([hovered] { return hovered.Get(); }, hoveredBackground.AsView(), HoverFade))
            .OnClick([action = Move(action)](UIPointerEvent &) { action(); });
    };
    const Signal<bool> maximized = _maximized;
    ApplicationSubsystem *application = _application;
    const auto window = [application] {
        return application != nullptr ? application->GetMainWindow() : WindowHandle();
    };
    return Row({
                   button("Minus", "WindowButton", [window] { PlatformWindow::Minimize(window()); }),
                   button("Square", "WindowButton", [window] { PlatformWindow::Maximize(window()); })
                       .Visible([maximized] { return !maximized.Get(); }),
                   button("Copy", "WindowButton", [window] { PlatformWindow::Restore(window()); }).Visible([maximized] {
                       return maximized.Get();
                   }),
                   button("X", "WindowButton.Close", [window] { PlatformWindow::RequestClose(window()); }),
               })
        .AlignSelf(UIAlign::Stretch)
        .Shrink(0);
}

UIElement EditorShellSubsystem::BuildMenuBar()
{
    MenuState *menu = _menu;
    EditorMenuSubsystem *menus = _menus;
    List<UIElement> labels;
    for (usize index = 0; index < EditorMenuSubsystem::MenuBar.Count(); ++index)
    {
        const auto open = static_cast<int32>(index);
        UIElement label = Box();
        const Signal<bool> hovered = TrackHover(label);
        const auto highlighted = [menu, hovered, open] { return hovered.Get() || menu->Open.Get() == open; };
        label = WithChildren(Move(label),
                             {Text(EditorMenuSubsystem::MenuBar[index])
                                  .FontSize(ThemeMetric("FontSize.Normal"))
                                  .TextColor([highlighted] {
                                      return ThemeColor(highlighted() ? "MenuBar.HoveredText" : "MenuBar.Text")();
                                  })})
                    .Height(ThemeMetric("MenuBar.ItemHeight"))
                    .Padding([] { return UIEdges(ThemeMetric("MenuBar.ItemPaddingX")(), 0); })
                    .Justify(UIJustify::Center)
                    .Background(FadeIn(highlighted, "MenuBar.Hovered", HoverFade));
        UIElementNode *node = label.GetNode();
        label.OnPointerDown([menu, menus, open, node](UIPointerEvent &event) {
            if (event.Button != MouseButton::Left ||
                menus->GetItems(EditorMenuSubsystem::MenuBar[static_cast<usize>(open)].AsView()).IsEmpty())
            {
                return;
            }
            const UIRect rect = LayoutRect(node);
            menu->Anchor.Set(Vector2(rect.X, rect.Y + rect.Height));
            menu->Open.Set(open);
        });
        menu->Labels.Add(node);
        labels.Add(label);
    }
    Reactive::OnCleanup([menu] { menu->Labels.Clear(); });

    const String &engineId = Engine::GetEnvironment().EngineId;
    StringView build = engineId.AsView();
    StringView prefix;
    StringView hash;
    if (build.SplitLast('-', prefix, hash))
    {
        build = hash;
    }
    UIElement items = WithChildren(Row().AlignItems(UIAlign::Center).Gap(ThemeMetric("MenuBar.Gap")), labels.AsSpan());
    UIElement bar = Row({
                            items,
                            Box().Grow(1),
                            Text(Format("Build: {0}", build))
                                .Font(_text != nullptr ? _text->GetMonospaceFaces() : Span<const UIFontFace>())
                                .FontSize(ThemeMetric("FontSize.Small"))
                                .TextColor(ThemeColor("Text.Secondary")),
                        })
                        .Grow(1)
                        .AlignItems(UIAlign::Center)
                        .Padding([] { return UIEdges(ThemeMetric("MenuBar.PaddingX")(), 0); });
    // The bar's bottom border is a line of its own, below its height.
    return Column({bar, Box().Height(1).Shrink(0).Background(ThemeColor("MenuBar.Border"))})
        .Height([] { return UILength(ThemeMetric("MenuBar.Height")() + 1); })
        .Shrink(0)
        .Background(ThemeColor("MenuBar.Background"));
}

UIElement EditorShellSubsystem::BuildMenuOverlay()
{
    MenuState *menu = _menu;
    // Over the whole window while a menu is open: a press outside the popup closes it, and moving onto another menu
    // bar label opens that menu instead, as menu bars do.
    const auto labelAt = [menu](Vector2 point) -> int32 {
        for (usize index = 0; index < menu->Labels.Count(); ++index)
        {
            const UIRect rect = LayoutRect(menu->Labels[index]);
            if (point.X >= rect.X && point.X < rect.X + rect.Width && point.Y >= rect.Y &&
                point.Y < rect.Y + rect.Height)
            {
                return static_cast<int32>(index);
            }
        }
        return -1;
    };
    EditorMenuSubsystem *menus = _menus;
    const auto openAt = [menu, menus](int32 index) {
        if (menus->GetItems(EditorMenuSubsystem::MenuBar[static_cast<usize>(index)].AsView()).IsEmpty())
        {
            return;
        }
        const UIRect rect = LayoutRect(menu->Labels[static_cast<usize>(index)]);
        menu->Anchor.Set(Vector2(rect.X, rect.Y + rect.Height));
        menu->Open.Set(index);
    };
    UIElement scrim = Box()
                          .Absolute(Vector2())
                          .Width(UILength::Percent(100))
                          .Height(UILength::Percent(100))
                          .WindowCaption()
                          .OnPointerMove([labelAt, openAt, menu](UIPointerEvent &event) {
                              const int32 index = labelAt(event.Position);
                              if (index >= 0 && index != menu->Open.Peek())
                              {
                                  openAt(index);
                              }
                          })
                          .OnPointerDown([labelAt, menu](UIPointerEvent &event) {
                              const int32 index = labelAt(event.Position);
                              menu->Open.Set(index == menu->Open.Peek() ? -1 : index < 0 ? -1 : index);
                              event.Handled = true;
                          });
    return Box({
                   scrim,
                   For<int32>(
                       [menu] {
                           List<int32> open;
                           if (menu->Open.Get() >= 0)
                           {
                               open.Add(menu->Open.Get());
                           }
                           return open;
                       },
                       [](const int32 &index) { return static_cast<uint64>(index); },
                       [this](Signal<int32> index) { return BuildMenuPopup(static_cast<usize>(index.Peek())); }),
               })
        .Absolute(Vector2())
        .Width(UILength::Percent(100))
        .Height(UILength::Percent(100))
        .Visible([menu] { return menu->Open.Get() >= 0; });
}

UIElement EditorShellSubsystem::BuildMenuPopup(usize menuIndex)
{
    MenuState *menu = _menu;
    const Span<const EditorMenuItem> items = _menus->GetItems(EditorMenuSubsystem::MenuBar[menuIndex].AsView());
    List<UIElement> rows;
    for (const EditorMenuItem &item : items)
    {
        if (item.IsVisible.IsBound() && !item.IsVisible())
        {
            continue;
        }
        const EditorMenuItem *entry = &item;
        const bool enabled = !item.IsEnabled.IsBound() || item.IsEnabled();
        UIElement row = Row();
        const Signal<bool> hovered = TrackHover(row);
        const auto labelColor = [hovered, enabled] {
            return ThemeColor(!enabled ? "Text.Disabled" : hovered.Get() ? "Menu.ItemHoveredText" : "Menu.Text")();
        };
        row = WithChildren(
                  Move(row),
                  {Text(item.Label).Grow(1).FontSize(ThemeMetric("FontSize.Normal")).TextColor(labelColor),
                   Text(item.Shortcut).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor("Menu.Shortcut"))})
                  .Gap(24)
                  .AlignItems(UIAlign::Center)
                  .Height(ThemeMetric("Menu.ItemHeight"))
                  .Padding(UIEdges(8, 0))
                  .Radius(ThemeMetric("Radius.Small"));
        if (enabled)
        {
            row.Background(FadeIn([hovered] { return hovered.Get(); }, "Menu.ItemHovered", HoverFade));
            row.OnPointerDown([](UIPointerEvent &event) { event.Handled = true; })
                .OnClick([menu, entry](UIPointerEvent &) {
                    menu->Open.Set(-1);
                    entry->Execute();
                });
        }
        rows.Add(row);
    }
    // Pops open: fades in, drops a few units and grows from its top-left corner, overshooting a little.
    const Tween<float32> open(0, MenuPop);
    open.Set(1);
    return WithChildren(Column(), rows.AsSpan())
        .Absolute([menu] { return menu->Anchor.Get(); })
        .Opacity([open] { return Math::Clamp(open.Get(), 0.0f, 1.0f); })
        .Translate([open] { return Vector2(0, (open.Get() - 1) * 4); })
        .Scale([open] { return 0.94f + 0.06f * open.Get(); })
        .MinWidth(ThemeMetric("Menu.MinWidth"))
        .Padding(UIEdges(3))
        .Background(ThemeColor("Menu.Background"))
        .Border(1, ThemeColor("Menu.Border"))
        .Radius(ThemeMetric("Radius.Medium"));
}

UIElement EditorShellSubsystem::BuildContent()
{
    EditingContextSubsystem *contexts = _contexts;
    return Box({For<EditingContext *>([contexts] { return contexts->GetContexts(); },
                                      [](EditingContext *const &context) { return reinterpret_cast<uint64>(context); },
                                      [contexts](Signal<EditingContext *> item) {
                                          EditingContext *context = item.Peek();
                                          // Built the first time the tab is shown, then kept while hidden.
                                          const Signal<bool> shown = CreateSignal(false);
                                          CreateEffect([contexts, context, shown] {
                                              if (contexts->GetActive() == context)
                                              {
                                                  shown.Set(true);
                                              }
                                          });
                                          return Box({Show([shown] { return shown.Get(); },
                                                           [context] { return context->BuildContent(); })})
                                              .Grow(1)
                                              .Visible(
                                                  [contexts, context] { return contexts->GetActive() == context; });
                                      })})
        .Grow(1)
        .Background(ThemeColor("Content.Background"));
}

UIElement EditorShellSubsystem::BuildStatusBar()
{
    // The left slot will hold the hint for what is under the pointer, the right one the frame counter.
    UIElement slots =
        Row({Box().Grow(1), Box()}).Grow(1).AlignItems(UIAlign::Center).Gap(ThemeMetric("StatusBar.Gap")).Padding([] {
            return UIEdges(ThemeMetric("StatusBar.PaddingX")(), 0);
        });
    return Column({Box().Height(1).Shrink(0).Background(ThemeColor("StatusBar.Border")), slots})
        .Height([] { return UILength(ThemeMetric("StatusBar.Height")() + 1); })
        .Shrink(0)
        .Background(ThemeColor("StatusBar.Background"));
}

} // namespace Marlin
