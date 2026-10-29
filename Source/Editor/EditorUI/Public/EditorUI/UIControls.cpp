#include "EditorUI/UIControls.h"

#include "Application/PlatformApplication.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/UIAnimation.h"
#include "EditorUI/UIWidgets.h"
#include "EditorUI/UIWindowRoot.h"
#include "Engine/Engine.h"
#include "Math/MathUtility.h"
#include "String/StringBuilder.h"
#include "String/Utf.h"
#include "UIPainter/UIPainter.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
namespace
{
constexpr UITransition ControlFade{0.08f, &Easing::EaseOutCubic};
/// How far one wheel notch scrolls a scroll view, in painter units.
constexpr float32 ScrollStep = 48;

/// *value* as a handle lambdas can copy: UIValue holds a move-only function.
template <typename T> Computed<T> Follow(UIValue<T> value)
{
    auto *held = new UIValue<T>(Move(value));
    Reactive::OnCleanup([held] { delete held; });
    return CreateComputed<T>([held] { return held->Get(); });
}

UICornerRadii Corners(float32 radius)
{
    return UICornerRadii(radius, radius, radius, radius);
}

/// The byte index of the code point before (or after) *index* in UTF-8 *text*.
usize PreviousCodePoint(StringView text, usize index)
{
    while (index > 0)
    {
        --index;
        if ((static_cast<uint8>(text[index]) & 0xC0) != 0x80)
        {
            break;
        }
    }
    return index;
}

usize NextCodePoint(StringView text, usize index)
{
    if (index >= text.Length())
    {
        return text.Length();
    }
    ++index;
    while (index < text.Length() && (static_cast<uint8>(text[index]) & 0xC0) == 0x80)
    {
        ++index;
    }
    return index;
}

/// The width of *text* as drawn at *size* in the default faces.
float32 TextWidth(StringView text, float32 size)
{
    const UITextSubsystem *subsystem = Engine::GetSubsystem<UITextSubsystem>();
    if (subsystem == nullptr || text.IsEmpty())
    {
        return 0;
    }
    UIShapedText shaped;
    subsystem->Shape(text, {subsystem->GetDefaultFaces(), size}, shaped);
    return shaped.Width;
}
} // namespace

UIElement Button(UIValue<String> label, Function<void()> onClick, UIButtonKind kind, UIValue<bool> enabled)
{
    const bool primary = kind == UIButtonKind::Primary;
    UIElement button = Box();
    const Signal<bool> hovered = TrackHover(button);
    const Computed<bool> isEnabled = Follow(Move(enabled));
    // Button.Background, Button.HoveredBackground, Button.DisabledBackground, and the same under Button.Primary.
    const auto token = [primary, hovered, isEnabled](StringView part) {
        StringBuilder<64> name(primary ? "Button.Primary." : "Button.");
        name.Append(!isEnabled.Get() ? "Disabled" : hovered.Get() ? "Hovered" : "").Append(part);
        return ThemeColor(name.AsView())();
    };
    return WithChildren(Move(button), {Text(Move(label)).FontSize(ThemeMetric("FontSize.Normal")).TextColor([token] {
                            return token("Text");
                        })})
        .Height(ThemeMetric("ControlHeight"))
        .Padding([] { return UIEdges(ThemeMetric("Button.PaddingX")(), 0); })
        .Justify(UIJustify::Center)
        .AlignItems(UIAlign::Center)
        .Shrink(0)
        .Radius([] { return Corners(ThemeMetric("Radius.Medium")()); })
        .Background([token] { return token("Background"); })
        .Border(1, [token] { return token("Border"); })
        .OnClick([isEnabled, onClick = Move(onClick)](UIPointerEvent &event) {
            if (isEnabled.Get() && event.Button == MouseButton::Left)
            {
                event.Handled = true;
                onClick();
            }
        });
}

UIElement IconButton(StringView icon, Function<void()> onClick, UIValue<bool> enabled, float32 size)
{
    UIElement button = Box();
    const Signal<bool> hovered = TrackHover(button);
    const Computed<bool> isEnabled = Follow(Move(enabled));
    return WithChildren(
               Move(button),
               {IconView(
                   icon, [isEnabled] { return ThemeColor(isEnabled.Get() ? "Button.Text" : "Button.DisabledText")(); },
                   size * 0.55f, 1.25f)})
        .Width(size)
        .Height(size)
        .Shrink(0)
        .Justify(UIJustify::Center)
        .AlignItems(UIAlign::Center)
        .Radius([] { return Corners(ThemeMetric("Radius.Medium")()); })
        .Background([hovered, isEnabled] {
            return ThemeColor(!isEnabled.Get() ? "Button.DisabledBackground"
                              : hovered.Get()  ? "Button.HoveredBackground"
                                               : "Button.Background")();
        })
        .Border(1, [isEnabled] { return ThemeColor(isEnabled.Get() ? "Button.Border" : "Button.DisabledBorder")(); })
        .OnClick([isEnabled, onClick = Move(onClick)](UIPointerEvent &event) {
            if (isEnabled.Get() && event.Button == MouseButton::Left)
            {
                event.Handled = true;
                onClick();
            }
        });
}

UIElement ToolbarButton(StringView icon, StringView label, Function<void()> onClick, UIValue<bool> enabled,
                        StringView iconColor)
{
    UIElement button = Column();
    const Signal<bool> hovered = TrackHover(button);
    const Computed<bool> isEnabled = Follow(Move(enabled));
    const String color(iconColor);
    return WithChildren(
               Move(button),
               {Box({IconView(
                        icon, [color] { return ThemeColor(color.AsView())(); }, ThemeMetric("Toolbar.IconSize")())})
                    .Width(22)
                    .Height(22)
                    .Justify(UIJustify::Center)
                    .AlignItems(UIAlign::Center),
                Text(String(label)).FontSize(ThemeMetric("FontSize.Small")).TextColor(ThemeColor("Toolbar.Label"))})
        .AlignItems(UIAlign::Center)
        .Gap(3)
        .Padding(UIEdges(8, 4, 8, 3))
        .MinWidth(ThemeMetric("Toolbar.ButtonMinWidth"))
        .Shrink(0)
        .Radius([] { return Corners(ThemeMetric("Radius.Medium")()); })
        .Opacity([isEnabled] { return isEnabled.Get() ? 1.0f : 0.4f; })
        .Background(FadeIn([hovered, isEnabled] { return hovered.Get() && isEnabled.Get(); }, "Toolbar.ButtonHovered",
                           ControlFade))
        .OnClick([isEnabled, onClick = Move(onClick)](UIPointerEvent &event) {
            if (isEnabled.Get() && event.Button == MouseButton::Left)
            {
                event.Handled = true;
                onClick();
            }
        });
}

UIElement ToolbarSeparator()
{
    return Box().Width(1).AlignSelf(UIAlign::Stretch).Margin(UIEdges(6, 2)).Background(ThemeColor("Toolbar.Separator"));
}

UIElement TextField(Signal<String> text, StringView placeholder, StringView icon)
{
    struct Editing
    {
        usize Caret = 0;
        /// When the caret last moved, so that it shows solid while typing and blinks once still.
        float64 Moved = 0;
    };
    auto *editing = new Editing;
    Reactive::OnCleanup([editing] { delete editing; });
    const Signal<bool> focused = CreateSignal(false);
    const auto setText = [text, editing](String next, usize caret) {
        editing->Caret = Min(caret, next.Length());
        editing->Moved = UIAnimation::PeekNow();
        text.Set(Move(next));
    };

    UIElement value = Text(text).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor("Input.Text"));
    UIElement hint = Text(String(placeholder))
                         .FontSize(ThemeMetric("FontSize.Normal"))
                         .TextColor(ThemeColor("Input.Placeholder"))
                         .Visible([text] { return text.Get().IsEmpty(); });
    UIElementNode *valueNode = value.GetNode();
    // The text, scrolled so the caret stays in view; the caret drawn over it.
    UIElement area =
        Row({value, hint})
            .Grow(1)
            .AlignItems(UIAlign::Center)
            .Clip()
            .OnPaint([text, focused, editing, valueNode](UIPainter &painter, const UIRect &rect) {
                const String &current = text.Peek();
                editing->Caret = Min(editing->Caret, current.Length());
                const float32 size = valueNode->GetFontSize();
                const float32 caret = TextWidth(current.AsView().Left(editing->Caret), size);
                const float32 scroll = Max(caret - rect.Width + 2, 0.0f);
                valueNode->Style.Translate = Vector2(-scroll, 0);
                if (!focused.Peek())
                {
                    return;
                }
                const float64 still = UIAnimation::PeekNow() - editing->Moved;
                if (still > 0.5 && static_cast<int64>(still / 0.5) % 2 == 1)
                {
                    return;
                }
                const float32 height = size * 1.25f;
                painter.FillRect({rect.X + caret - scroll, rect.Y + (rect.Height - height) * 0.5f, 1, height},
                                 ThemeColor("Input.Caret")());
            });

    UIElement clear =
        Show([text] { return !text.Get().IsEmpty(); },
             [setText] {
                 UIElement button = Box();
                 const Signal<bool> hovered = TrackHover(button);
                 return WithChildren(
                            Move(button),
                            {IconView(
                                "X", [hovered] { return ThemeColor(hovered.Get() ? "Input.Text" : "Input.Icon")(); },
                                ThemeMetric("Input.IconSize")() - 1, 1.5f)})
                     .Shrink(0)
                     .OnPointerDown([](UIPointerEvent &event) { event.Handled = true; })
                     .OnClick([setText](UIPointerEvent &event) {
                         event.Handled = true;
                         setText(String(), 0);
                     });
             });

    UIElement field = Row();
    if (!icon.IsEmpty())
    {
        field.GetNode()->AppendChild(
            IconView(icon, ThemeColor("Input.Icon"), ThemeMetric("Input.IconSize")(), 1.25f).GetNode());
    }
    field.GetNode()->AppendChild(area.GetNode());
    field.GetNode()->AppendChild(clear.GetNode());
    return field.Height(ThemeMetric("ControlHeight"))
        .AlignItems(UIAlign::Center)
        .Gap(6)
        .Padding(UIEdges(6, 0))
        .Radius([] { return Corners(ThemeMetric("Radius.Medium")()); })
        .Background(ThemeColor("Input.Background"))
        .Border(1, [focused] { return ThemeColor(focused.Get() ? "Input.FocusedBorder" : "Input.Border")(); })
        .Focusable()
        .OnFocusChanged([focused, editing, text](bool on) {
            focused.Set(on);
            editing->Caret = text.Peek().Length();
            editing->Moved = UIAnimation::PeekNow();
        })
        .OnTextInput([text, editing, setText](UITextInputEvent &event) {
            if (event.Character < 0x20 || event.Character == 0x7F)
            {
                return;
            }
            event.Handled = true;
            char encoded[4];
            const usize length = Utf8::Encode(static_cast<char32_t>(event.Character), encoded);
            String next = text.Peek();
            next.Insert(editing->Caret, StringView(encoded, length));
            setText(Move(next), editing->Caret + length);
        })
        .OnKeyDown([text, editing, setText](UIKeyEvent &event) {
            const String &current = text.Peek();
            const usize caret = Min(editing->Caret, current.Length());
            event.Handled = true;
            switch (event.Key)
            {
            case KeyCode::Backspace:
                if (caret > 0)
                {
                    const usize start = PreviousCodePoint(current.AsView(), caret);
                    String next = current;
                    next.Remove(start, caret - start);
                    setText(Move(next), start);
                }
                break;
            case KeyCode::Delete:
                if (caret < current.Length())
                {
                    String next = current;
                    next.Remove(caret, NextCodePoint(current.AsView(), caret) - caret);
                    setText(Move(next), caret);
                }
                break;
            case KeyCode::Left:
                editing->Caret = PreviousCodePoint(current.AsView(), caret);
                editing->Moved = UIAnimation::PeekNow();
                break;
            case KeyCode::Right:
                editing->Caret = NextCodePoint(current.AsView(), caret);
                editing->Moved = UIAnimation::PeekNow();
                break;
            case KeyCode::Home:
                editing->Caret = 0;
                editing->Moved = UIAnimation::PeekNow();
                break;
            case KeyCode::End:
                editing->Caret = current.Length();
                editing->Moved = UIAnimation::PeekNow();
                break;
            case KeyCode::V:
                if ((event.Modifiers & ModifierKeys::Control) != ModifierKeys::None)
                {
                    char pasted[1024];
                    const usize length =
                        Min(PlatformApplication::GetClipboardText(pasted, sizeof(pasted)), sizeof(pasted) - 1);
                    // One line: stop at the first line break.
                    usize end = 0;
                    while (end < length && pasted[end] != '\n' && pasted[end] != '\r' && pasted[end] != '\0')
                    {
                        ++end;
                    }
                    String next = current;
                    next.Insert(caret, StringView(pasted, end));
                    setText(Move(next), caret + end);
                }
                else
                {
                    event.Handled = false;
                }
                break;
            default:
                event.Handled = false;
                break;
            }
        })
        .OnPointerDown([](UIPointerEvent &event) {
            if (event.Button == MouseButton::Left)
            {
                event.Handled = true;
            }
        });
}

UIElement Chip(StringView icon, UIValue<LinearColor> iconColor, UIValue<String> label, UIValue<bool> active,
               Function<void()> onClick)
{
    UIElement chip = Row();
    const Signal<bool> hovered = TrackHover(chip);
    const Computed<bool> isActive = Follow(Move(active));
    const auto on = [isActive] { return isActive.Get(); };
    return WithChildren(Move(chip), {IconView(icon, Move(iconColor), ThemeMetric("Chip.IconSize")(), 1.25f),
                                     Text(Move(label)).FontSize(ThemeMetric("FontSize.Normal")).TextColor([on] {
                                         return ThemeColor(on() ? "Chip.ActiveText" : "Chip.Text")();
                                     })})
        .Height(ThemeMetric("Chip.Height"))
        .Gap(5)
        .Padding(UIEdges(8, 0))
        .AlignItems(UIAlign::Center)
        .Shrink(0)
        .Radius([] { return Corners(ThemeMetric("Radius.Medium")()); })
        .Background([on, hovered] {
            return ThemeColor(on()            ? "Chip.ActiveBackground"
                              : hovered.Get() ? "Chip.HoveredBackground"
                                              : "Chip.Background")();
        })
        .Border(1, [on] { return ThemeColor(on() ? "Chip.ActiveBorder" : "Chip.Border")(); })
        .OnClick([onClick = Move(onClick)](UIPointerEvent &event) {
            if (event.Button == MouseButton::Left)
            {
                event.Handled = true;
                onClick();
            }
        });
}

UIElement Slider(Signal<float32> value, float32 min, float32 max)
{
    const Signal<bool> held = CreateSignal(false);
    const auto setFrom = [value, min, max](const UIPointerEvent &event, float32 width) {
        const float32 knob = ThemeMetric("Slider.KnobSize")();
        const float32 travel = Max(width - knob, 1.0f);
        const float32 amount = Math::Clamp((event.LocalPosition.X - knob * 0.5f) / travel, 0.0f, 1.0f);
        value.Set(min + (max - min) * amount);
    };
    UIElement slider = Box();
    UIElementNode *node = slider.GetNode();
    return slider.Height(14)
        .Shrink(0)
        .OnPaint([value, min, max](UIPainter &painter, const UIRect &rect) {
            const float32 knob = ThemeMetric("Slider.KnobSize")();
            const float32 amount = max > min ? Math::Clamp((value.Get() - min) / (max - min), 0.0f, 1.0f) : 0.0f;
            const float32 x = rect.X + knob * 0.5f + (rect.Width - knob) * amount;
            const float32 y = rect.Y + rect.Height * 0.5f;
            painter.FillRect({rect.X, y - 1, rect.Width, 2}, ThemeColor("Slider.Track")());
            painter.FillRect({rect.X, y - 1, x - rect.X, 2}, ThemeColor("Slider.Fill")());
            painter.FillEllipse({x, y}, {knob * 0.5f, knob * 0.5f}, ThemeColor("Slider.KnobBorder")());
            painter.FillEllipse({x, y}, {knob * 0.5f - 2, knob * 0.5f - 2}, ThemeColor("Slider.Knob")());
        })
        .OnPointerDown([held, setFrom, node](UIPointerEvent &event) {
            if (event.Button != MouseButton::Left)
            {
                return;
            }
            event.Handled = true;
            held.Set(true);
            setFrom(event, node->GetBounds().Width);
        })
        .OnPointerMove([held, setFrom, node](UIPointerEvent &event) {
            if (held.Peek())
            {
                setFrom(event, node->GetBounds().Width);
            }
        })
        .OnPointerUp([held](UIPointerEvent &) { held.Set(false); });
}

namespace
{
/// *parts* are icon tokens, or with *labels* texts.
UIElement Segmented(List<String> parts, bool labels, Signal<int32> selected)
{
    UIElement row = Row();
    for (usize index = 0; index < parts.Count(); ++index)
    {
        const auto mine = static_cast<int32>(index);
        UIElement item = Box();
        const Signal<bool> hovered = TrackHover(item);
        const auto color = [selected, mine] {
            return ThemeColor(selected.Get() == mine ? "Segmented.ActiveText" : "Segmented.Text")();
        };
        UIElement part = labels ? Text(parts[index]).FontSize(ThemeMetric("FontSize.Small")).TextColor(color)
                                : IconView(parts[index].AsView(), color, ThemeMetric("Segmented.IconSize")(), 1.25f);
        item = WithChildren(Move(item), {part})
                   .Height(ThemeMetric("Segmented.Height"))
                   .MinWidth(ThemeMetric("Segmented.Height"))
                   .Padding(UIEdges(6, 0))
                   .Justify(UIJustify::Center)
                   .AlignItems(UIAlign::Center)
                   .Background([selected, mine, hovered] {
                       return ThemeColor(selected.Get() == mine ? "Segmented.Active"
                                         : hovered.Get()        ? "Segmented.Hovered"
                                                                : "Segmented.Background")();
                   })
                   .OnClick([selected, mine](UIPointerEvent &event) {
                       event.Handled = true;
                       selected.Set(mine);
                   });
        row.GetNode()->AppendChild(item.GetNode());
    }
    return row.Shrink(0)
        .Clip()
        .Radius([] { return Corners(ThemeMetric("Radius.Medium")()); })
        .Border(1, ThemeColor("Segmented.Border"));
}
} // namespace

UIElement SegmentedIcons(std::initializer_list<StringView> icons, Signal<int32> selected)
{
    List<String> parts;
    for (const StringView icon : icons)
    {
        parts.Add(String(icon));
    }
    return Segmented(Move(parts), false, selected);
}

UIElement SegmentedLabels(std::initializer_list<StringView> labels, Signal<int32> selected)
{
    List<String> parts;
    for (const StringView label : labels)
    {
        parts.Add(String(label));
    }
    return Segmented(Move(parts), true, selected);
}

UIElement ScrollView(UIElement content)
{
    struct Scroll
    {
        float32 Offset = 0;
        UIElementNode *Content = nullptr;
    };
    auto *scroll = new Scroll;
    Reactive::OnCleanup([scroll] { delete scroll; });
    scroll->Content = content.GetNode();
    content.Shrink(0);
    // Drawn over the content without taking up room or presses: a thumb along the right edge while it overflows.
    UIElement thumb = Box().Absolute(Vector2()).OnPaint([scroll](UIPainter &painter, const UIRect &) {
        const UIElementNode *view = scroll->Content->GetParent();
        if (view == nullptr)
        {
            return;
        }
        const UIRect &bounds = view->GetBounds();
        const float32 total = scroll->Content->GetBounds().Height;
        if (total <= bounds.Height)
        {
            return;
        }
        const Vector2 origin = view->GetWindowPosition();
        const float32 length = Max(bounds.Height * bounds.Height / total, 16.0f);
        const float32 position = (bounds.Height - length) * scroll->Offset / (total - bounds.Height);
        const float32 width = ThemeMetric("Scrollbar.Width")();
        painter.FillRoundedRect({origin.X + bounds.Width - width - 2, origin.Y + position, width, length},
                                Corners(width * 0.5f), ThemeColor("Scrollbar.Thumb")());
    });
    return Column({content, thumb})
        .Clip()
        .OnWheel([scroll](UIPointerEvent &event) {
            scroll->Offset -= event.Wheel.Y * ScrollStep;
            event.Handled = true;
        })
        .OnPaint([scroll](UIPainter &, const UIRect &rect) {
            // Before the content paints: clamp to this layout and apply.
            const float32 overflow = Max(scroll->Content->GetBounds().Height - rect.Height, 0.0f);
            scroll->Offset = Math::Clamp(scroll->Offset, 0.0f, overflow);
            scroll->Content->Style.Translate = Vector2(0, -scroll->Offset);
        });
}

UIElement Section(UIValue<String> title, UIElement content, bool open)
{
    const Signal<bool> expanded = CreateSignal(open);
    UIElement header =
        Row({
                Box({IconView("ChevronDown", ThemeColor("Section.Caret"), 10, 1.5f).Visible(expanded),
                     IconView("ChevronRight", ThemeColor("Section.Caret"), 10, 1.5f).Visible([expanded] {
                         return !expanded.Get();
                     })}),
                Text(Move(title)).FontSize(ThemeMetric("FontSize.Normal")).TextColor(ThemeColor("Section.Text")),
            })
            .Height(ThemeMetric("Section.HeaderHeight"))
            .Shrink(0)
            .Gap(6)
            .Padding(UIEdges(6, 0))
            .AlignItems(UIAlign::Center)
            .Background(ThemeColor("Section.Header"))
            .OnClick([expanded](UIPointerEvent &event) {
                event.Handled = true;
                expanded.Set(!expanded.Peek());
            });
    content.Visible([expanded] { return expanded.Get(); });
    return Column({Box().Height(1).Shrink(0).Background(ThemeColor("Section.Border")), header, content}).Shrink(0);
}

void ShowContextMenu(const UIElementNode *origin, Vector2 position, List<UIMenuItem> items)
{
    UIWindowRoot *window = UIWindowRoot::Find(origin);
    if (window == nullptr)
    {
        return;
    }
    auto *held = new List<UIMenuItem>(Move(items));
    window->ShowPopup(position, [window, held] {
        Reactive::OnCleanup([held] { delete held; });
        UIElement menu = Column();
        for (const UIMenuItem &item : *held)
        {
            if (item.Separator)
            {
                menu.GetNode()->AppendChild(
                    Box().Height(1).Margin(UIEdges(0, 3)).Background(ThemeColor("Menu.Separator")).GetNode());
                continue;
            }
            const UIMenuItem *entry = &item;
            const bool enabled = item.Enabled && item.Execute.IsBound();
            UIElement row = Row();
            const Signal<bool> hovered = TrackHover(row);
            const auto color = [hovered, enabled] {
                return ThemeColor(!enabled ? "Text.Disabled" : hovered.Get() ? "Menu.ItemHoveredText" : "Menu.Text")();
            };
            UIElement icon = item.Icon.IsEmpty() ? Box().Width(13).Height(13).Shrink(0)
                                                 : IconView(item.Icon.AsView(), color, 13, 1.25f);
            row =
                WithChildren(Move(row),
                             {icon, Text(item.Label).Grow(1).FontSize(ThemeMetric("FontSize.Normal")).TextColor(color),
                              Text(item.Shortcut)
                                  .FontSize(ThemeMetric("FontSize.Small"))
                                  .TextColor(ThemeColor("Menu.Shortcut"))})
                    .Gap(8)
                    .AlignItems(UIAlign::Center)
                    .Height(ThemeMetric("Menu.ItemHeight"))
                    .Padding(UIEdges(8, 0))
                    .Radius([] { return Corners(ThemeMetric("Radius.Small")()); });
            if (enabled)
            {
                row.Background(FadeIn([hovered] { return hovered.Get(); }, "Menu.ItemHovered", ControlFade))
                    .OnPointerDown([](UIPointerEvent &event) { event.Handled = true; })
                    .OnClick([window, entry](UIPointerEvent &event) {
                        event.Handled = true;
                        window->ClosePopup();
                        entry->Execute();
                    });
            }
            menu.GetNode()->AppendChild(row.GetNode());
        }
        return menu.MinWidth(ThemeMetric("Menu.MinWidth"))
            .Padding(UIEdges(3))
            .Background(ThemeColor("Menu.Background"))
            .Border(1, ThemeColor("Menu.Border"))
            .Radius([] { return Corners(ThemeMetric("Radius.Medium")()); });
    });
}
} // namespace Marlin
