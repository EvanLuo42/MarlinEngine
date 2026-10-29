#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/Signal.h"
#include "EditorUI/UIAnimation.h"
#include "EditorUI/UIElement.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// Icon *token* of the current theme (EditorTheme::GetIcon), *size* painter units square, drawn in *color*.
/// *strokeWidth* > 0 draws every stroke that wide in painter units instead of the icon's own widths scaled. Draws
/// nothing until the icon has loaded.
EDITORUI_API UIElement IconView(StringView token, UIValue<LinearColor> color, float32 size = 16,
                                float32 strokeWidth = 0);

/// *element* with *children* appended: for children built in a loop, which an initializer list cannot hold.
EDITORUI_API UIElement WithChildren(UIElement element, Span<const UIElement> children);
EDITORUI_API UIElement WithChildren(UIElement element, std::initializer_list<UIElement> children);

/// Theme color *token* while *on* holds, its alpha fading in and out over *transition*: hover and press highlights.
[[nodiscard]] inline auto FadeIn(Function<bool()> on, StringView token, UITransition transition)
{
    const auto amount = Animate<float32>([on = Move(on)] { return on() ? 1.0f : 0.0f; }, transition);
    return [amount, token = String(token)] {
        LinearColor color = ThemeColor(token.AsView())();
        color.A *= amount();
        return color;
    };
}

/// Whether the pointer is over *element* or its children; sets its OnPointerEnter and OnPointerLeave.
[[nodiscard]] EDITORUI_API Signal<bool> TrackHover(UIElement &element);

/// Whether *element* is held down with the left button; sets its OnPointerDown and OnPointerUp.
[[nodiscard]] EDITORUI_API Signal<bool> TrackPress(UIElement &element);
} // namespace Marlin
