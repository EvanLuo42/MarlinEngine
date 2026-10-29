#pragma once

#include "Application/PlatformApplication.h"
#include "Application/PlatformWindow.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorUI/Signal.h"
#include "EditorUI/UIElement.h"
#include "Math/Vector.h"
#include "Templates/Function.h"

namespace Marlin
{
class UIPainter;
class UITextSubsystem;

/// The UI of one OS window (09 §4.1): owns its content, dispatches the window's input to it, lays it
/// out (only the subtrees marked dirty) and paints it with UIPainter. A frame runs DispatchEvent for the window's
/// events, Reactive::Flush, Layout and Paint, in that order. Main thread only.
class EDITORUI_API UIWindowRoot
{
public:
    /// *text* shapes Text elements; without it text measures as empty.
    explicit UIWindowRoot(const UITextSubsystem *text);
    ~UIWindowRoot();
    MARLIN_NONCOPYABLE(UIWindowRoot);

    /// Replaces the content with what *build* returns, built under the window's owner.
    void SetContent(Function<UIElement()> build);

    /// Input for this window; positions are physical pixels, divided by *scale* into painter units.
    void DispatchEvent(const ApplicationEvent &event, float32 scale);
    /// Lays the tree out in *size* painter units.
    void Layout(Vector2 size);
    void Paint(UIPainter &painter) const;

    /// The deepest element under *point* (painter units), or null.
    [[nodiscard]] UIElementNode *HitTest(Vector2 point) const;

    [[nodiscard]] UIElementNode *GetRoot() const
    {
        return _root;
    }

    [[nodiscard]] UIElementNode *GetFocused() const
    {
        return _focused;
    }

    void SetFocus(UIElementNode *node);

    /// Shows what *build* returns over everything else at *position* (painter units), such as a context menu, built
    /// under an owner of its own; a press outside it or Escape closes it. Replaces an open one.
    void ShowPopup(Vector2 position, Function<UIElement()> build);
    /// Closes the popup at the start of the next layout, so that a handler inside it can call this.
    void ClosePopup();
    [[nodiscard]] bool HasPopup() const
    {
        return _popup != nullptr;
    }

    /// The window whose tree holds *node*, or null.
    [[nodiscard]] static UIWindowRoot *Find(const UIElementNode *node);

    /// For a window with its own title bar: the WindowCaption elements as Caption regions, and the elements in them
    /// that take pointer presses as Client regions after them, in client pixels (PlatformWindow::SetHitTestRegions).
    void CollectHitRegions(float32 scale, List<WindowHitRegion> &regions) const;

    /// The OS window, so that a press keeps the mouse until it is released outside the window.
    void SetWindowHandle(WindowHandle window)
    {
        _windowHandle = window;
    }

    [[nodiscard]] WindowHandle GetWindowHandle() const
    {
        return _windowHandle;
    }

    /// Text that was shaped while its font loads; it is shaped and laid out again next frame.
    void OnShapedBeforeReady(UIElementNode *node);
    /// Whether some text still waits for its font.
    [[nodiscard]] bool HasPendingText() const
    {
        return !_pendingText.IsEmpty();
    }
    void OnNodeDestroyed(UIElementNode *node);

private:
    static void PaintNode(UIPainter &painter, const UIElementNode &node, Vector2 parentOrigin);
    static UIElementNode *HitNode(UIElementNode &node, Vector2 point, Vector2 parentOrigin);
    static void CollectNodeRegions(const UIElementNode &node, Vector2 parentOrigin, float32 scale, bool inCaption,
                                   List<WindowHitRegion> &regions);
    void Release(Vector2 position);
    void UpdateHover(UIElementNode *target, const UIPointerEvent &event);
    /// Runs *handler* of each element from *target* up to the root until one handles the event.
    template <typename TEvent>
    void Bubble(UIElementNode *target, Function<void(TEvent &)> UIElementNode::*handler, TEvent &event);

    const UITextSubsystem *_text = nullptr;
    WindowHandle _windowHandle;
    ReactiveOwner *_owner = nullptr;
    ReactiveOwner *_contentOwner = nullptr;
    UIElementNode *_root = nullptr;
    Vector2 _size;
    Vector2 _pointer{-1e6f, -1e6f};
    /// From the deepest element to the root.
    List<UIElementNode *> _hovered;
    UIElementNode *_pressed = nullptr;
    MouseButton _pressedButton = MouseButton::Left;
    /// The window holds the mouse for a press, which outlives the pressed element if that is freed.
    bool _capturing = false;
    UIElementNode *_focused = nullptr;
    /// The chain an event is bubbling up; entries freed meanwhile become null.
    List<UIElementNode *> _dispatch;
    /// Text shaped while its font was loading, shaped again next layout.
    List<UIElementNode *> _pendingText;
    /// The popup's layer over the content, its own element and owner, and whether it closes at the next layout.
    UIElementNode *_popup = nullptr;
    UIElementNode *_popupContent = nullptr;
    ReactiveOwner *_popupOwner = nullptr;
    bool _closePopup = false;
    /// The last press, for counting the clicks of a double click.
    UIElementNode *_lastPress = nullptr;
    MouseButton _lastPressButton = MouseButton::Left;
    Vector2 _lastPressPosition;
    float64 _lastPressTime = -1;
    uint32 _clicks = 1;
};
} // namespace Marlin
