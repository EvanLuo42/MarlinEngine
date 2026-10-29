#include "EditorUI/UIAnimation.h"
#include "EditorUI/UIElement.h"
#include "EditorUI/UIWindowRoot.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
bool Near(float32 a, float32 b)
{
    return a - b < 0.01f && b - a < 0.01f;
}

bool RectIs(const UIElementNode *node, float32 x, float32 y, float32 width, float32 height)
{
    const UIRect &bounds = node->GetBounds();
    return Near(bounds.X, x) && Near(bounds.Y, y) && Near(bounds.Width, width) && Near(bounds.Height, height);
}

ApplicationEvent Mouse(ApplicationEventType type, int32 x, int32 y)
{
    ApplicationEvent event;
    event.Type = type;
    event.X = x;
    event.Y = y;
    return event;
}
} // namespace

MARLIN_TEST(EditorUIElement, FlexRowSizesAndJustifies)
{
    UIWindowRoot window(nullptr);
    UIElementNode *a = nullptr;
    UIElementNode *b = nullptr;
    UIElementNode *c = nullptr;
    window.SetContent([&] {
        UIElement first = Box().Width(100).Height(20);
        UIElement second = Box().Grow(1).Margin(UIEdges(5, 0));
        UIElement third = Box().Width(50).AlignSelf(UIAlign::Center).Height(10);
        a = first.GetNode();
        b = second.GetNode();
        c = third.GetNode();
        return Row({first, second, third}).Gap(10).Padding(UIEdges(4)).Height(48);
    });
    window.Layout({400, 300});
    MARLIN_EXPECT(RectIs(a, 4, 4, 100, 20));
    // RECOVERY-DECISION D5: never run before the loss; 392 inner - 100 - 50 - 2 gaps - 10 margin leaves 212 (c's x
    // needs it).
    MARLIN_EXPECT(RectIs(b, 119, 4, 212, 40));
    MARLIN_EXPECT(RectIs(c, 346, 19, 50, 10));
}

MARLIN_TEST(EditorUIElement, JustifySpaceBetweenAndShrink)
{
    UIWindowRoot window(nullptr);
    UIElementNode *first = nullptr;
    UIElementNode *last = nullptr;
    UIElementNode *wide = nullptr;
    UIElementNode *narrow = nullptr;
    window.SetContent([&] {
        UIElement left = Box().Width(30).Height(10);
        UIElement right = Box().Width(30).Height(10);
        first = left.GetNode();
        last = right.GetNode();
        UIElement shrinkA = Box().Basis(200).Height(10);
        UIElement shrinkB = Box().Basis(100).Height(10);
        wide = shrinkA.GetNode();
        narrow = shrinkB.GetNode();
        return Column(
            {Row({left, right}).Justify(UIJustify::SpaceBetween).Width(100), Row({shrinkA, shrinkB}).Width(150)});
    });
    window.Layout({400, 300});
    MARLIN_EXPECT(RectIs(first, 0, 0, 30, 10) && RectIs(last, 70, 0, 30, 10));
    MARLIN_EXPECT(Near(wide->GetBounds().Width, 100) && Near(narrow->GetBounds().Width, 50));
}

MARLIN_TEST(EditorUIElement, GridTracksAndSpans)
{
    UIWindowRoot window(nullptr);
    List<UIElementNode *> cells;
    window.SetContent([&] {
        UIElement header = Box().Cell(1, 1, 3).Height(20);
        UIElement left = Box().Height(30);
        UIElement middle = Box().Height(10);
        UIElement right = Box().Height(10);
        cells.Add(header.GetNode());
        cells.Add(left.GetNode());
        cells.Add(middle.GetNode());
        cells.Add(right.GetNode());
        return Grid({header, left, middle, right})
            .Columns({UIGridTrack::Points(100), UIGridTrack::Fraction(1), UIGridTrack::Fraction(3)})
            .Gap(10)
            .Width(520);
    });
    window.Layout({800, 600});
    MARLIN_EXPECT(RectIs(cells[0], 0, 0, 520, 20));
    MARLIN_EXPECT(RectIs(cells[1], 0, 30, 100, 30));
    // RECOVERY-DECISION D5: an explicit Height beats stretch, as in flex; the row is 30 high but these stay 10.
    MARLIN_EXPECT(RectIs(cells[2], 110, 30, 100, 10));
    MARLIN_EXPECT(RectIs(cells[3], 220, 30, 300, 10));
}

MARLIN_TEST(EditorUIElement, AutoFillGridsWrapAsManyColumnsAsFit)
{
    UIWindowRoot window(nullptr);
    List<UIElementNode *> tiles;
    window.SetContent([&] {
        UIElement grid = Grid().AutoFillColumns(100).Gap(10).Width(330);
        for (int32 index = 0; index < 4; ++index)
        {
            UIElement tile = Box().Height(40);
            tiles.Add(tile.GetNode());
            grid.GetNode()->AppendChild(tile.GetNode());
        }
        return Row({grid});
    });
    window.Layout({800, 600});
    // (330 + 10) / (100 + 10) = 3 columns of (330 - 20) / 3; the fourth tile wraps.
    const float32 width = 310.0f / 3;
    MARLIN_EXPECT(RectIs(tiles[0], 0, 0, width, 40));
    MARLIN_EXPECT(RectIs(tiles[2], (width + 10) * 2, 0, width, 40));
    MARLIN_EXPECT(RectIs(tiles[3], 0, 50, width, 40));
}

MARLIN_TEST(EditorUIElement, BoundPropertiesRelayout)
{
    UIWindowRoot window(nullptr);
    Signal<float32> width;
    UIElementNode *after = nullptr;
    window.SetContent([&] {
        width = UseState(40.0f);
        UIElement second = Box().Width(10).Height(10);
        after = second.GetNode();
        return Row({Box().Width(width).Height(10), second});
    });
    window.Layout({400, 300});
    MARLIN_EXPECT(Near(after->GetBounds().X, 40));
    width.Set(70);
    Reactive::Flush();
    window.Layout({400, 300});
    MARLIN_EXPECT(Near(after->GetBounds().X, 70));
}

MARLIN_TEST(EditorUIElement, ShowBuildsAndFreesContent)
{
    UIWindowRoot window(nullptr);
    Signal<bool> open;
    int32 built = 0;
    int32 freed = 0;
    UIElementNode *after = nullptr;
    window.SetContent([&] {
        open = UseState(false);
        UIElement tail = Box().Height(5);
        after = tail.GetNode();
        return Column({Show(open,
                            [&] {
                                ++built;
                                Reactive::OnCleanup([&] { ++freed; });
                                return Box().Height(30);
                            }),
                       tail});
    });
    window.Layout({100, 100});
    MARLIN_EXPECT(built == 0 && Near(after->GetBounds().Y, 0));
    open.Set(true);
    Reactive::Flush();
    window.Layout({100, 100});
    MARLIN_EXPECT(built == 1 && Near(after->GetBounds().Y, 30));
    open.Set(false);
    Reactive::Flush();
    window.Layout({100, 100});
    MARLIN_EXPECT(freed == 1 && Near(after->GetBounds().Y, 0));
}

MARLIN_TEST(EditorUIElement, SwitchTakesTheFirstMatch)
{
    UIWindowRoot window(nullptr);
    Signal<int32> mode;
    UIElementNode *fragment = nullptr;
    window.SetContent([&] {
        mode = UseState(0);
        UIElement chosen = Switch(Match([=] { return mode.Get() == 1; }, [] { return Box().Height(10); }),
                                  Match([=] { return mode.Get() >= 1; }, [] { return Box().Height(20); }),
                                  Default([] { return Box().Height(30); }));
        fragment = chosen.GetNode();
        return Column({chosen});
    });
    window.Layout({100, 100});
    MARLIN_EXPECT(fragment->GetChildren().Count() == 1 && Near(fragment->GetChildren()[0]->GetBounds().Height, 30));
    mode.Set(2);
    Reactive::Flush();
    window.Layout({100, 100});
    MARLIN_EXPECT(Near(fragment->GetChildren()[0]->GetBounds().Height, 20));
    mode.Set(1);
    Reactive::Flush();
    window.Layout({100, 100});
    MARLIN_EXPECT(Near(fragment->GetChildren()[0]->GetBounds().Height, 10));
}

MARLIN_TEST(EditorUIElement, ForKeepsRowsByKey)
{
    struct Item
    {
        uint64 Id = 0;
        float32 Size = 0;
        bool operator==(const Item &) const = default;
    };
    UIWindowRoot window(nullptr);
    Signal<List<Item>> items;
    UIElementNode *list = nullptr;
    int32 built = 0;
    int32 freed = 0;
    window.SetContent([&] {
        List<Item> initial;
        initial.Add({1, 10});
        initial.Add({2, 20});
        initial.Add({3, 30});
        items = UseState(Move(initial));
        UIElement rows = For<Item>(
            items, [](const Item &item) { return item.Id; },
            [&](Signal<Item> item) {
                ++built;
                Reactive::OnCleanup([&] { ++freed; });
                return Box().Height([item] { return UILength(item.Get().Size); });
            });
        list = rows.GetNode();
        return Column({rows});
    });
    window.Layout({100, 200});
    const UIElementNode *two = list->GetChildren()[1];
    MARLIN_EXPECT(built == 3 && Near(two->GetBounds().Y, 10));
    items.Update([](List<Item> &value) {
        value.RemoveAt(0);
        value.Add({1, 15});
        value[0].Size = 25;
    });
    Reactive::Flush();
    window.Layout({100, 200});
    MARLIN_EXPECT(built == 3 && freed == 0 && list->GetChildren()[0] == two);
    MARLIN_EXPECT(Near(two->GetBounds().Height, 25) && Near(list->GetChildren()[2]->GetBounds().Y, 55));
    items.Set(List<Item>());
    Reactive::Flush();
    MARLIN_EXPECT(freed == 3 && list->GetChildren().IsEmpty());
}

MARLIN_TEST(EditorUIElement, PointerEventsBubbleAndHover)
{
    UIWindowRoot window(nullptr);
    int32 innerClicks = 0;
    int32 outerClicks = 0;
    int32 enters = 0;
    int32 leaves = 0;
    bool stop = false;
    UIElementNode *inner = nullptr;
    window.SetContent([&] {
        UIElement button = Box()
                               .Width(50)
                               .Height(20)
                               .Focusable()
                               .OnClick([&](UIPointerEvent &event) {
                                   ++innerClicks;
                                   event.Handled = stop;
                               })
                               .OnPointerEnter([&](UIPointerEvent &) { ++enters; })
                               .OnPointerLeave([&](UIPointerEvent &) { ++leaves; });
        inner = button.GetNode();
        return Box({button}).Padding(UIEdges(10)).OnClick([&](UIPointerEvent &) { ++outerClicks; });
    });
    window.Layout({200, 200});
    window.DispatchEvent(Mouse(ApplicationEventType::MouseMove, 20, 15), 1);
    MARLIN_EXPECT(enters == 1 && window.HitTest({20, 15}) == inner);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonDown, 20, 15), 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonUp, 22, 16), 1);
    MARLIN_EXPECT(innerClicks == 1 && outerClicks == 1 && window.GetFocused() == inner);
    stop = true;
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonDown, 20, 15), 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonUp, 20, 15), 1);
    MARLIN_EXPECT(innerClicks == 2 && outerClicks == 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonDown, 20, 15), 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonUp, 150, 150), 1);
    MARLIN_EXPECT(innerClicks == 2 && leaves == 1 && window.GetFocused() == inner);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonDown, 150, 150), 1);
    MARLIN_EXPECT(window.GetFocused() == nullptr);
}

MARLIN_TEST(EditorUIElement, QuickSecondPressesCountAsDoubleClicks)
{
    UIAnimation::SetTime(10);
    UIWindowRoot window(nullptr);
    List<uint32> clicks;
    window.SetContent([&] {
        return Box(
            {Box().Width(50).Height(20).OnPointerDown([&](UIPointerEvent &event) { clicks.Add(event.Clicks); })});
    });
    window.Layout({200, 200});
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonDown, 10, 10), 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonUp, 10, 10), 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonDown, 11, 10), 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonUp, 11, 10), 1);
    // Later, it starts over.
    UIAnimation::SetTime(11);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonDown, 10, 10), 1);
    window.DispatchEvent(Mouse(ApplicationEventType::MouseButtonUp, 10, 10), 1);
    MARLIN_REQUIRE(clicks.Count() == 3);
    MARLIN_EXPECT(clicks[0] == 1 && clicks[1] == 2 && clicks[2] == 1);
}
MARLIN_TEST(EditorUIElement, ClippedOutSubtreesAreNotPainted)
{
    // Ten clipping rows of 20 in a clipping column of 50: the rows from y 60 on are skipped with their children.
    UIWindowRoot window(nullptr);
    uint32 painted = 0;
    window.SetContent([&] {
        UIElement column = Column().Height(50).Clip();
        for (uint32 index = 0; index < 10; ++index)
        {
            UIElement row = Box({Box().Height(10).OnPaint([&](UIPainter &, const UIRect &) { ++painted; })})
                                .Height(20)
                                .Shrink(0)
                                .Clip();
            column.GetNode()->AppendChild(row.GetNode());
        }
        return column;
    });
    window.Layout({100, 100});
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 100, 100, 1);
    window.Paint(painter);
    painter.End();
    MARLIN_EXPECT(painted == 3);
}

MARLIN_TEST(EditorUIElement, MeasureIsStableUnderAlternatingOffers)
{
    // A grid of wrapping-free text-less boxes measured under several widths in turn lays out the same each time.
    UIWindowRoot window(nullptr);
    UIElementNode *grid = nullptr;
    UIElementNode *cell = nullptr;
    window.SetContent([&] {
        UIElement first = Box().Width(30).Height(12);
        cell = first.GetNode();
        UIElement element = Box({first, Box().Width(40).Height(8), Box().Width(10).Height(16)});
        element.GetNode()->Style.Layout = UILayoutKind::Grid;
        element.GetNode()->Style.AutoFillColumnWidth = 40;
        element.GetNode()->Style.Gap = 2;
        grid = element.GetNode();
        return Row({element.Grow(1)});
    });
    window.Layout({200, 100});
    const UIRect wide = grid->GetBounds();
    const UIRect wideCell = cell->GetBounds();
    window.Layout({90, 100});
    const UIRect narrow = grid->GetBounds();
    window.Layout({200, 100});
    MARLIN_EXPECT(grid->GetBounds() == wide && cell->GetBounds() == wideCell);
    window.Layout({90, 100});
    MARLIN_EXPECT(grid->GetBounds() == narrow);
}
} // namespace Marlin::Tests
