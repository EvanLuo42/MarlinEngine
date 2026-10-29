#include "EditorUI/UIWidgets.h"

#include "EditorUI/EditorTheme.h"
#include "UIIcon/Icon.h"

namespace Marlin
{
UIElement IconView(StringView token, UIValue<LinearColor> color, float32 size, float32 strokeWidth)
{
    const Name icon(token);
    return Box().Width(size).Height(size).Shrink(0).OnPaint(
        [icon, color = Move(color), strokeWidth, cache = IconDrawCache()](UIPainter &painter,
                                                                          const UIRect &rect) mutable {
            const EditorTheme *theme = EditorTheme::GetCurrent();
            if (const Icon *loaded = theme != nullptr ? theme->GetIcon(icon) : nullptr)
            {
                cache.Draw(painter, *loaded, rect, color.Get(), strokeWidth);
            }
        });
}

UIElement WithChildren(UIElement element, Span<const UIElement> children)
{
    for (const UIElement &child : children)
    {
        if (child.GetNode() != nullptr)
        {
            element.GetNode()->AppendChild(child.GetNode());
        }
    }
    return element;
}

UIElement WithChildren(UIElement element, std::initializer_list<UIElement> children)
{
    return WithChildren(Move(element), Span<const UIElement>(children.begin(), children.size()));
}

Signal<bool> TrackHover(UIElement &element)
{
    Signal<bool> hovered = CreateSignal(false);
    element.OnPointerEnter([hovered](UIPointerEvent &) { hovered.Set(true); })
        .OnPointerLeave([hovered](UIPointerEvent &) { hovered.Set(false); });
    return hovered;
}

Signal<bool> TrackPress(UIElement &element)
{
    Signal<bool> pressed = CreateSignal(false);
    element
        .OnPointerDown([pressed](UIPointerEvent &event) {
            if (event.Button == MouseButton::Left)
            {
                pressed.Set(true);
            }
        })
        .OnPointerUp([pressed](UIPointerEvent &event) {
            if (event.Button == MouseButton::Left)
            {
                pressed.Set(false);
            }
        });
    return pressed;
}
} // namespace Marlin
