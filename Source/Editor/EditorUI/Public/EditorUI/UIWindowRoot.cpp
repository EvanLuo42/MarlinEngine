#include "EditorUI/UIWindowRoot.h"

#include "EditorUI/UIAnimation.h"
#include "EditorUI/UILayout.h"
#include "EditorUI/UIWindowRegistry.h"
#include "UIPainter/UIPainter.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
namespace
{
List<UIWindowRoot *> &GetWindows()
{
    static List<UIWindowRoot *> windows;
    return windows;
}

bool Contains(const UIRect &rect, Vector2 point)
{
    return point.X >= rect.X && point.Y >= rect.Y && point.X < rect.X + rect.Width && point.Y < rect.Y + rect.Height;
}

bool IsWithin(const UIElementNode *node, const UIElementNode *ancestor)
{
    for (; node != nullptr; node = node->GetParent())
    {
        if (node == ancestor)
        {
            return true;
        }
    }
    return false;
}
} // namespace

namespace Private
{
void RegisterWindow(UIWindowRoot *window)
{
    GetWindows().Add(window);
}

void UnregisterWindow(UIWindowRoot *window)
{
    List<UIWindowRoot *> &windows = GetWindows();
    for (usize index = 0; index < windows.Count(); ++index)
    {
        if (windows[index] == window)
        {
            windows.RemoveAtSwap(index);
            return;
        }
    }
}

void NotifyNodeDestroyed(UIElementNode *node)
{
    for (UIWindowRoot *window : GetWindows())
    {
        window->OnNodeDestroyed(node);
    }
}
} // namespace Private

UIWindowRoot::UIWindowRoot(const UITextSubsystem *text) : _text(text)
{
    Private::RegisterWindow(this);
    _owner = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(_owner, [this] { _root = Box().GetNode(); });
}

UIWindowRoot::~UIWindowRoot()
{
    Private::UnregisterWindow(this);
    Reactive::DisposeOwner(_owner);
}

void UIWindowRoot::SetContent(Function<UIElement()> build)
{
    if (_contentOwner != nullptr)
    {
        Reactive::DisposeOwner(_contentOwner);
    }
    _contentOwner = Reactive::CreateOwner(_owner);
    UIElementNode *content = nullptr;
    Reactive::RunInOwner(_contentOwner, [&] { content = build().GetNode(); });
    _root->SetChildren(content != nullptr ? Span<UIElementNode *const>(&content, 1) : Span<UIElementNode *const>());
}

void UIWindowRoot::ShowPopup(Vector2 position, Function<UIElement()> build)
{
    if (_popupOwner != nullptr)
    {
        Reactive::DisposeOwner(_popupOwner);
    }
    _popupOwner = Reactive::CreateOwner(_owner);
    _closePopup = false;
    Reactive::RunInOwner(_popupOwner, [&] {
        UIElement content = build();
        content.Absolute(position);
        _popupContent = content.GetNode();
        // Takes presses outside the popup: they close it rather than reaching what is under it.
        UIElement scrim = Box()
                              .Absolute(Vector2())
                              .Width(UILength::Percent(100))
                              .Height(UILength::Percent(100))
                              .OnPointerDown([this](UIPointerEvent &event) {
                                  event.Handled = true;
                                  ClosePopup();
                              });
        _popup = Box({scrim, content})
                     .Absolute(Vector2())
                     .Width(UILength::Percent(100))
                     .Height(UILength::Percent(100))
                     .GetNode();
    });
    _root->AppendChild(_popup);
}

void UIWindowRoot::ClosePopup()
{
    _closePopup = _popup != nullptr;
}

UIWindowRoot *UIWindowRoot::Find(const UIElementNode *node)
{
    while (node != nullptr && node->GetParent() != nullptr)
    {
        node = node->GetParent();
    }
    for (UIWindowRoot *window : GetWindows())
    {
        if (node != nullptr && window->_root == node)
        {
            return window;
        }
    }
    return nullptr;
}

void UIWindowRoot::Layout(Vector2 size)
{
    if (_closePopup)
    {
        _closePopup = false;
        _popup = nullptr;
        _popupContent = nullptr;
        Reactive::DisposeOwner(_popupOwner);
        _popupOwner = nullptr;
    }
    for (UIElementNode *node : _pendingText)
    {
        node->MarkLayoutDirty();
    }
    _pendingText.Clear();
    _size = size;
    const UILayout layout{_text, this};
    layout.Arrange(*_root, {0, 0, size.X, size.Y});
    if (_popupContent != nullptr)
    {
        // Keep it in the window: past the right or bottom edge, move it back in for the next frame.
        const UIRect &bounds = _popupContent->GetBounds();
        Vector2 offset = _popupContent->Style.Offset;
        offset.X = Max(Min(offset.X, size.X - bounds.Width), 0.0f);
        offset.Y = Max(Min(offset.Y, size.Y - bounds.Height), 0.0f);
        if (offset.X != _popupContent->Style.Offset.X || offset.Y != _popupContent->Style.Offset.Y)
        {
            _popupContent->Style.Offset = offset;
            _popupContent->MarkLayoutDirty();
            layout.Arrange(*_root, {0, 0, size.X, size.Y});
        }
    }
}

void UIWindowRoot::Paint(UIPainter &painter) const
{
    PaintNode(painter, *_root, {});
}

void UIWindowRoot::PaintNode(UIPainter &painter, const UIElementNode &node, Vector2 parentOrigin)
{
    if (!node.Style.Visible)
    {
        return;
    }
    const Vector2 origin = parentOrigin + Vector2(node._bounds.X, node._bounds.Y) + node.Style.Translate;
    if (node._kind == UIElementKind::Fragment)
    {
        for (const UIElementNode *child : node._children)
        {
            PaintNode(painter, *child, origin);
        }
        return;
    }
    const UIStyle &style = node.Style;
    if (style.Opacity <= 0)
    {
        return;
    }
    // A clipping element bounds what it and its children paint, unless it paints itself (text and OnPaint may overflow
    // before the clip is pushed), so one wholly outside the painter's clip is skipped with its subtree.
    if (style.Clip && style.Scale == 1 && node._kind != UIElementKind::Text && !node.OnPaint.IsBound() &&
        painter.IsClippedOut({origin.X, origin.Y, node._bounds.Width, node._bounds.Height}))
    {
        return;
    }
    const bool scaled = style.Scale != 1;
    if (scaled)
    {
        painter.PushTransform(UITransform::Translation(-origin.X, -origin.Y)
                                  .Then(UITransform::Scale(style.Scale, style.Scale))
                                  .Then(UITransform::Translation(origin.X, origin.Y)));
    }
    const bool layer = style.Opacity < 1;
    if (layer)
    {
        painter.PushLayer(style.Opacity);
    }
    const UIRect rect(origin.X, origin.Y, node._bounds.Width, node._bounds.Height);
    if (style.Background.A > 0)
    {
        painter.FillRoundedRect(rect, style.Radius, style.Background);
    }
    if (node._kind == UIElementKind::Text && !node._shaped.Runs.IsEmpty())
    {
        DrawText(painter, node._shaped, origin + Vector2(style.Padding.Left, style.Padding.Top + node._shaped.Ascent),
                 node.TextColor);
    }
    if (node.OnPaint.IsBound())
    {
        node.OnPaint(painter, rect);
    }
    if (style.Clip)
    {
        painter.PushClip(rect, style.Radius);
    }
    for (const UIElementNode *child : node._children)
    {
        PaintNode(painter, *child, origin);
    }
    if (style.Clip)
    {
        painter.PopClip();
    }
    if (style.BorderWidth > 0 && style.BorderColor.A > 0)
    {
        const float32 half = style.BorderWidth * 0.5f;
        painter.StrokeRoundedRect(
            {rect.X + half, rect.Y + half, rect.Width - style.BorderWidth, rect.Height - style.BorderWidth},
            style.Radius, style.BorderWidth, style.BorderColor);
    }
    if (layer)
    {
        painter.PopLayer();
    }
    if (scaled)
    {
        painter.PopTransform();
    }
}

UIElementNode *UIWindowRoot::HitTest(Vector2 point) const
{
    return _root != nullptr ? HitNode(*_root, point, {}) : nullptr;
}

UIElementNode *UIWindowRoot::HitNode(UIElementNode &node, Vector2 point, Vector2 parentOrigin)
{
    if (!node.Style.Visible)
    {
        return nullptr;
    }
    const Vector2 origin = parentOrigin + Vector2(node._bounds.X, node._bounds.Y) + node.Style.Translate;
    if (node.Style.Scale != 1 && node.Style.Scale > 0)
    {
        // Into the unscaled space of the element and its subtree.
        point = origin + (point - origin) / node.Style.Scale;
    }
    const bool fragment = node._kind == UIElementKind::Fragment;
    const bool inside = !fragment && Contains({origin.X, origin.Y, node._bounds.Width, node._bounds.Height}, point);
    if (!fragment && !inside && node.Style.Clip)
    {
        return nullptr;
    }
    for (usize index = node._children.Count(); index-- > 0;)
    {
        if (UIElementNode *hit = HitNode(*node._children[index], point, origin))
        {
            return hit;
        }
    }
    return inside ? &node : nullptr;
}

void UIWindowRoot::CollectHitRegions(float32 scale, List<WindowHitRegion> &regions) const
{
    regions.Clear();
    if (_root != nullptr)
    {
        CollectNodeRegions(*_root, {}, scale, false, regions);
    }
}

void UIWindowRoot::CollectNodeRegions(const UIElementNode &node, Vector2 parentOrigin, float32 scale, bool inCaption,
                                      List<WindowHitRegion> &regions)
{
    if (!node.Style.Visible)
    {
        return;
    }
    const Vector2 origin = parentOrigin + Vector2(node._bounds.X, node._bounds.Y) + node.Style.Translate;
    const bool fragment = node._kind == UIElementKind::Fragment;
    const bool pressable = node.OnPointerDown.IsBound() || node.OnPointerUp.IsBound() || node.OnClick.IsBound();
    if (!fragment && (node.WindowCaption || (inCaption && pressable)))
    {
        WindowHitRegion region;
        region.Rect = {static_cast<int32>(origin.X * scale), static_cast<int32>(origin.Y * scale),
                       static_cast<int32>(node._bounds.Width * scale + 0.5f),
                       static_cast<int32>(node._bounds.Height * scale + 0.5f)};
        region.Area = pressable ? WindowHitArea::Client : WindowHitArea::Caption;
        regions.Add(region);
    }
    // A pressable element takes the presses of its whole subtree.
    const bool childrenInCaption = (inCaption || node.WindowCaption) && !pressable;
    for (const UIElementNode *child : node._children)
    {
        CollectNodeRegions(*child, origin, scale, childrenInCaption, regions);
    }
}

void UIWindowRoot::SetFocus(UIElementNode *node)
{
    if (node == _focused)
    {
        return;
    }
    UIElementNode *previous = _focused;
    _focused = node;
    if (previous != nullptr && previous->OnFocusChanged.IsBound())
    {
        previous->OnFocusChanged(false);
    }
    if (_focused != nullptr && _focused->OnFocusChanged.IsBound())
    {
        _focused->OnFocusChanged(true);
    }
}

template <typename TEvent>
void UIWindowRoot::Bubble(UIElementNode *target, Function<void(TEvent &)> UIElementNode::*handler, TEvent &event)
{
    _dispatch.Clear();
    for (UIElementNode *node = target; node != nullptr; node = node->GetParent())
    {
        _dispatch.Add(node);
    }
    for (usize index = 0; index < _dispatch.Count() && !event.Handled; ++index)
    {
        UIElementNode *node = _dispatch[index];
        if (node == nullptr || !(node->*handler).IsBound())
        {
            continue;
        }
        // RECOVERY-DECISION D2: this guard was lost to carve junk; only events with a LocalPosition get one.
        if constexpr (requires { event.LocalPosition; })
        {
            event.LocalPosition = event.Position - node->GetWindowPosition();
        }
        (node->*handler)(event);
    }
    _dispatch.Clear();
}

void UIWindowRoot::UpdateHover(UIElementNode *target, const UIPointerEvent &event)
{
    List<UIElementNode *> chain;
    for (UIElementNode *node = target; node != nullptr; node = node->GetParent())
    {
        chain.Add(node);
    }
    List<UIElementNode *> previous = Move(_hovered);
    _hovered = chain;
    for (UIElementNode *node : previous)
    {
        if (!chain.Contains(node) && node->OnPointerLeave.IsBound())
        {
            UIPointerEvent leave = event;
            leave.LocalPosition = event.Position - node->GetWindowPosition();
            node->OnPointerLeave(leave);
        }
    }
    for (usize index = chain.Count(); index-- > 0;)
    {
        UIElementNode *node = chain[index];
        if (!previous.Contains(node) && node->OnPointerEnter.IsBound())
        {
            UIPointerEvent enter = event;
            enter.LocalPosition = event.Position - node->GetWindowPosition();
            node->OnPointerEnter(enter);
        }
    }
}

void UIWindowRoot::Release(Vector2 position)
{
    _pressed = nullptr;
    _capturing = false;
    if (_windowHandle.IsValid())
    {
        PlatformApplication::SetMouseCapture(_windowHandle, false);
    }
    UIPointerEvent event;
    event.Position = position;
    UpdateHover(HitTest(position), event);
}

void UIWindowRoot::DispatchEvent(const ApplicationEvent &event, float32 scale)
{
    const Vector2 position = Vector2(static_cast<float32>(event.X), static_cast<float32>(event.Y)) / scale;
    UIPointerEvent pointer;
    pointer.Modifiers = event.Modifiers;
    switch (event.Type)
    {
    case ApplicationEventType::MouseMove: {
        _pointer = position;
        pointer.Position = position;
        if (_pressed == nullptr)
        {
            UpdateHover(HitTest(position), pointer);
        }
        Bubble(_pressed != nullptr ? _pressed : HitTest(position), &UIElementNode::OnPointerMove, pointer);
        break;
    }
    case ApplicationEventType::MouseButtonDown: {
        _pointer = position;
        pointer.Position = position;
        pointer.Button = event.Button;
        UIElementNode *target = HitTest(position);
        // A press soon after another of the same button nearby counts as its next click.
        const float64 now = UIAnimation::PeekNow();
        const Vector2 moved = position - _lastPressPosition;
        const bool again = target == _lastPress && event.Button == _lastPressButton && now - _lastPressTime < 0.4 &&
                           moved.X * moved.X + moved.Y * moved.Y < 16;
        _clicks = again ? _clicks + 1 : 1;
        _lastPress = target;
        _lastPressButton = event.Button;
        _lastPressPosition = position;
        _lastPressTime = now;
        pointer.Clicks = _clicks;
        if (_pressed == nullptr)
        {
            _pressed = target;
            _pressedButton = event.Button;
            if (_windowHandle.IsValid())
            {
                PlatformApplication::SetMouseCapture(_windowHandle, true);
                _capturing = true;
            }
        }
        UIElementNode *focus = target;
        while (focus != nullptr && !focus->Focusable)
        {
            focus = focus->GetParent();
        }
        SetFocus(focus);
        Bubble(target, &UIElementNode::OnPointerDown, pointer);
        break;
    }
    case ApplicationEventType::MouseButtonUp: {
        _pointer = position;
        pointer.Position = position;
        pointer.Button = event.Button;
        pointer.Clicks = _clicks;
        UIElementNode *pressed = _pressed;
        Bubble(pressed != nullptr ? pressed : HitTest(position), &UIElementNode::OnPointerUp, pointer);
        if (pressed == nullptr && _capturing && event.Button == _pressedButton)
        {
            // The pressed element was freed while held: still give the mouse back.
            Release(position);
        }
        else if (pressed != nullptr && event.Button == _pressedButton)
        {
            const bool click = IsWithin(HitTest(position), pressed);
            Release(position);
            if (click)
            {
                UIPointerEvent clicked = pointer;
                Bubble(pressed, &UIElementNode::OnClick, clicked);
            }
        }
        break;
    }
    case ApplicationEventType::MouseWheel:
        pointer.Position = _pointer;
        pointer.Wheel = Vector2(event.WheelX, event.WheelY);
        Bubble(HitTest(_pointer), &UIElementNode::OnWheel, pointer);
        break;
    case ApplicationEventType::MouseLeave:
        if (_pressed == nullptr)
        {
            pointer.Position = _pointer;
            UpdateHover(nullptr, pointer);
        }
        break;
    case ApplicationEventType::KeyDown:
        if (_popup != nullptr && event.Key == KeyCode::Escape)
        {
            ClosePopup();
        }
        else if (_focused != nullptr)
        {
            UIKeyEvent key;
            key.Key = event.Key;
            key.Modifiers = event.Modifiers;
            key.Repeat = event.Repeat;
            Bubble(_focused, &UIElementNode::OnKeyDown, key);
        }
        break;
    case ApplicationEventType::TextInput:
        if (_focused != nullptr)
        {
            UITextInputEvent text;
            text.Character = event.Character;
            Bubble(_focused, &UIElementNode::OnTextInput, text);
        }
        break;
    default:
        break;
    }
}

void UIWindowRoot::OnShapedBeforeReady(UIElementNode *node)
{
    if (!_pendingText.Contains(node))
    {
        _pendingText.Add(node);
    }
}

void UIWindowRoot::OnNodeDestroyed(UIElementNode *node)
{
    for (usize index = _hovered.Count(); index-- > 0;)
    {
        if (_hovered[index] == node)
        {
            _hovered.RemoveAt(index);
        }
    }
    for (UIElementNode *&entry : _dispatch)
    {
        if (entry == node)
        {
            entry = nullptr;
        }
    }
    for (usize index = _pendingText.Count(); index-- > 0;)
    {
        if (_pendingText[index] == node)
        {
            _pendingText.RemoveAtSwap(index);
        }
    }
    if (_pressed == node)
    {
        _pressed = nullptr;
    }
    if (_focused == node)
    {
        _focused = nullptr;
    }
    if (_root == node)
    {
        _root = nullptr;
    }
    if (_lastPress == node)
    {
        _lastPress = nullptr;
    }
}
} // namespace Marlin
