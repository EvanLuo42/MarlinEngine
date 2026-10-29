#include "EditorUI/UIElement.h"
#include "EditorUI/UIWindowRoot.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
bool OverlayRectIs(const UIElementNode *node, float32 x, float32 y, float32 width, float32 height)
{
    const UIRect &bounds = node->GetBounds();
    return bounds.X == x && bounds.Y == y && bounds.Width == width && bounds.Height == height;
}

bool OverlayRegionIs(const WindowHitRegion &region, WindowHitArea area, int32 x, int32 y, int32 width, int32 height)
{
    return region.Area == area && region.Rect.X == x && region.Rect.Y == y && region.Rect.Width == width &&
           region.Rect.Height == height;
}
} // namespace

MARLIN_TEST(EditorUIOverlay, AbsoluteItemsLeaveTheFlow)
{
    UIWindowRoot window(nullptr);
    UIElementNode *first = nullptr;
    UIElementNode *popup = nullptr;
    UIElementNode *second = nullptr;
    window.SetContent([&] {
        UIElement a = Box().Height(20);
        UIElement overlay = Box().Width(50).Height(30).Absolute(Vector2(10, 5));
        UIElement b = Box().Height(20);
        first = a.GetNode();
        popup = overlay.GetNode();
        second = b.GetNode();
        return Column({a, overlay, b}).Padding(UIEdges(2));
    });
    window.Layout({200, 100});
    MARLIN_EXPECT(OverlayRectIs(first, 2, 2, 196, 20));
    MARLIN_EXPECT(OverlayRectIs(second, 2, 22, 196, 20));
    MARLIN_EXPECT(OverlayRectIs(popup, 12, 7, 50, 30));
    // Painted last among its siblings' order but hit first where it overlaps them.
    MARLIN_EXPECT(window.HitTest({20, 15}) == popup);
}

MARLIN_TEST(EditorUIOverlay, HiddenItemsKeepStateButTakeNoSpace)
{
    UIWindowRoot window(nullptr);
    Signal<bool> shown;
    UIElementNode *hidden = nullptr;
    UIElementNode *after = nullptr;
    window.SetContent([&] {
        shown = CreateSignal(false);
        UIElement a = Box().Height(20).Visible(shown);
        UIElement b = Box().Height(20);
        hidden = a.GetNode();
        after = b.GetNode();
        return Column({a, b});
    });
    window.Layout({100, 100});
    MARLIN_EXPECT(OverlayRectIs(after, 0, 0, 100, 20) && window.HitTest({5, 5}) == after);
    shown.Set(true);
    Reactive::Flush();
    window.Layout({100, 100});
    MARLIN_EXPECT(OverlayRectIs(after, 0, 20, 100, 20) && window.HitTest({5, 5}) == hidden);
}

MARLIN_TEST(EditorUIOverlay, HiddenAbsoluteItemsShowWhenTheyBecomeVisible)
{
    UIWindowRoot window(nullptr);
    Signal<bool> shown;
    UIElementNode *popup = nullptr;
    window.SetContent([&] {
        shown = CreateSignal(false);
        UIElement overlay = Box().Width(40).Height(30).Absolute(Vector2(10, 10)).Visible(shown);
        popup = overlay.GetNode();
        return Column({Box().Height(20), overlay});
    });
    window.Layout({100, 100});
    MARLIN_EXPECT(window.HitTest({20, 20}) != popup);
    shown.Set(true);
    Reactive::Flush();
    window.Layout({100, 100});
    MARLIN_EXPECT(OverlayRectIs(popup, 10, 10, 40, 30) && window.HitTest({20, 20}) == popup);
}

MARLIN_TEST(EditorUIOverlay, TranslateMovesPaintAndHitsNotLayout)
{
    UIWindowRoot window(nullptr);
    UIElementNode *moved = nullptr;
    window.SetContent([&] {
        UIElement box = Box().Width(10).Height(10).Translate(Vector2(30, 0));
        moved = box.GetNode();
        return Row({box});
    });
    window.Layout({100, 100});
    MARLIN_EXPECT(OverlayRectIs(moved, 0, 0, 10, 10));
    MARLIN_EXPECT(window.HitTest({35, 5}) == moved && window.HitTest({5, 5}) != moved);
    MARLIN_EXPECT(moved->GetWindowPosition() == Vector2(30, 0));
}

MARLIN_TEST(EditorUIOverlay, CaptionRegionsExceptPressableChildren)
{
    UIWindowRoot window(nullptr);
    window.SetContent([&] {
        UIElement button = Box().Width(20).OnClick([](UIPointerEvent &) {});
        UIElement hoverOnly = Box().Width(20).OnPointerEnter([](UIPointerEvent &) {});
        UIElement titleBar = Row({button, hoverOnly}).Height(30).WindowCaption();
        UIElement body = Box().Grow(1).OnClick([](UIPointerEvent &) {});
        return Column({titleBar, body}).Grow(1);
    });
    window.Layout({100, 80});
    List<WindowHitRegion> regions;
    window.CollectHitRegions(2, regions);
    // The title bar drags; the button in it takes presses; elements outside the title bar are left to the default.
    MARLIN_REQUIRE(regions.Count() == 2);
    MARLIN_EXPECT(OverlayRegionIs(regions[0], WindowHitArea::Caption, 0, 0, 200, 60));
    MARLIN_EXPECT(OverlayRegionIs(regions[1], WindowHitArea::Client, 0, 0, 40, 60));
}
} // namespace Marlin::Tests
