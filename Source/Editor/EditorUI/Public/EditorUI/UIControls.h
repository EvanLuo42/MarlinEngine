#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorUI/Signal.h"
#include "EditorUI/UIElement.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
/// The editor's common controls (Documentation/Architecture/09 §4.1), drawn from theme tokens as in the design's
/// component sheet: buttons, toolbar buttons, text fields, chips, sliders, segmented buttons, scroll views,
/// collapsible sections and context menus.

enum class UIButtonKind : uint8
{
    /// Gray, for most actions.
    Secondary,
    /// Accent-filled, for the one action a panel leads with.
    Primary,
};

/// A text button 24 units high; *enabled* false grays it out and ignores presses.
EDITORUI_API UIElement Button(UIValue<String> label, Function<void()> onClick,
                              UIButtonKind kind = UIButtonKind::Secondary, UIValue<bool> enabled = true);

/// A square button of *size* holding icon *icon*.
EDITORUI_API UIElement IconButton(StringView icon, Function<void()> onClick, UIValue<bool> enabled = true,
                                  float32 size = 24);

/// A toolbar button: an icon over a label, filling on hover. *iconColor* names the icon's color token.
EDITORUI_API UIElement ToolbarButton(StringView icon, StringView label, Function<void()> onClick,
                                     UIValue<bool> enabled = true, StringView iconColor = "Toolbar.Icon");

/// A vertical line between groups of toolbar buttons.
EDITORUI_API UIElement ToolbarSeparator();

/// A one-line text field editing *text*: click to focus, type, Backspace and Delete, the arrows, Home and End,
/// Ctrl+V to paste, Escape to leave. *placeholder* shows while it is empty, *icon* at its left, and an ✕ that clears
/// it at its right while it is not.
EDITORUI_API UIElement TextField(Signal<String> text, StringView placeholder = {}, StringView icon = {});

/// A filter chip: icon *icon* in *iconColor* and *label*, highlighted while *active*.
EDITORUI_API UIElement Chip(StringView icon, UIValue<LinearColor> iconColor, UIValue<String> label,
                            UIValue<bool> active, Function<void()> onClick);

/// A horizontal slider of *value* between *min* and *max*, dragged by its knob or by pressing the track.
EDITORUI_API UIElement Slider(Signal<float32> value, float32 min, float32 max);

/// Buttons side by side of which the one at *selected* is on; each shows an icon token.
EDITORUI_API UIElement SegmentedIcons(std::initializer_list<StringView> icons, Signal<int32> selected);

/// The same with labels.
EDITORUI_API UIElement SegmentedLabels(std::initializer_list<StringView> labels, Signal<int32> selected);

/// A vertical scroll view: *content* at its natural height, scrolled by the wheel, with a thin thumb showing where.
/// Give it a size or Grow it.
EDITORUI_API UIElement ScrollView(UIElement content);

/// A collapsible section: a header with a caret and *title* over *content*, which shows while it is open.
EDITORUI_API UIElement Section(UIValue<String> title, UIElement content, bool open = true);

/// One entry of a context menu: an item, or a separator.
struct UIMenuItem
{
    String Label;
    /// Icon token; empty for none.
    String Icon;
    /// Shown right-aligned, such as "Ctrl+C".
    String Shortcut;
    Function<void()> Execute;
    bool Enabled = true;
    bool Separator = false;
};

/// A line between groups of menu items.
[[nodiscard]] inline UIMenuItem MenuSeparator()
{
    UIMenuItem separator;
    separator.Separator = true;
    return separator;
}

/// Opens a menu of *items* at *position* (painter units) in the window that holds *origin*, such as the element
/// that was right-clicked; choosing an item runs it and closes the menu.
EDITORUI_API void ShowContextMenu(const UIElementNode *origin, Vector2 position, List<UIMenuItem> items);
} // namespace Marlin
