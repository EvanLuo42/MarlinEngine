#include "EditorUI/UILayout.h"

#include "EditorUI/UIWindowRoot.h"
#include "Math/MathUtility.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
namespace
{
constexpr float32 Unbounded = 1e30f;

/// The length in points, or -1 for Auto and for a percentage of unbounded space.
float32 Resolve(UILength length, float32 available)
{
    switch (length.Kind)
    {
    case UILength::Unit::Points:
        return length.Value;
    case UILength::Unit::Percent:
        return available < Unbounded ? available * length.Value * 0.01f : -1.0f;
    case UILength::Unit::Auto:
        break;
    }
    return -1.0f;
}

float32 Clamp(float32 value, float32 min, float32 max)
{
    return Max(Min(value, max), min);
}

float32 Shrunk(float32 available, float32 amount)
{
    return available < Unbounded ? Max(available - amount, 0.0f) : Unbounded;
}

/// The visible children of *node* in the flow (or, with *absolute*, out of it), looking through fragments, which are
/// placed at the origin of *node*.
void CollectItems(UIElementNode &node, List<UIElementNode *> &items, Vector2 size, bool absolute = false)
{
    for (UIElementNode *child : node.GetChildren())
    {
        if (child->GetKind() == UIElementKind::Fragment)
        {
            UILayout::PlaceFragment(*child, size);
            CollectItems(*child, items, size, absolute);
        }
        else if (child->Style.Visible && child->Style.Absolute == absolute)
        {
            items.Add(child);
        }
    }
}

struct FlexItem
{
    UIElementNode *Node = nullptr;
    float32 Base = 0;
    float32 Main = 0;
    float32 MarginMainStart = 0;
    float32 MarginMainEnd = 0;
    float32 MarginCrossStart = 0;
    float32 MarginCrossEnd = 0;
};
} // namespace

void UILayout::PlaceFragment(UIElementNode &fragment, Vector2 size)
{
    fragment._bounds = {0, 0, size.X, size.Y};
    fragment._layoutDirty = false;
    fragment._measureValid = true;
    fragment._measureCount = 0;
}

Vector2 UILayout::Measure(UIElementNode &node, Vector2 available) const
{
    if (node._measureValid)
    {
        for (uint32 index = 0; index < node._measureCount; ++index)
        {
            if (node._measureCache[index].Available == available)
            {
                return node._measureCache[index].Size;
            }
        }
    }
    const UIStyle &style = node.Style;
    float32 width = Resolve(style.Width, available.X);
    float32 height = Resolve(style.Height, available.Y);
    if (width < 0 || height < 0)
    {
        const Vector2 inner(Shrunk(width >= 0 ? width : available.X, style.Padding.Horizontal()),
                            Shrunk(height >= 0 ? height : available.Y, style.Padding.Vertical()));
        const Vector2 content = MeasureContent(node, inner);
        width = width >= 0 ? width : content.X + style.Padding.Horizontal();
        height = height >= 0 ? height : content.Y + style.Padding.Vertical();
    }
    const Vector2 measured(Clamp(width, style.MinWidth, style.MaxWidth),
                           Clamp(height, style.MinHeight, style.MaxHeight));
    if (!node._measureValid)
    {
        node._measureCount = 0;
        node._measureNext = 0;
        node._measureValid = true;
    }
    node._measureCache[node._measureNext] = {available, measured};
    node._measureNext = static_cast<uint8>((node._measureNext + 1) % UIElementNode::MeasureCacheSize);
    node._measureCount = static_cast<uint8>(Min<uint32>(node._measureCount + 1u, UIElementNode::MeasureCacheSize));
    return measured;
}

Vector2 UILayout::MeasureText(UIElementNode &node) const
{
    if (!node._shapeValid)
    {
        node._shaped = {};
        bool ready = true;
        if (Text != nullptr)
        {
            const Span<const UIFontFace> faces = node._faces.IsEmpty() ? Text->GetDefaultFaces() : node._faces;
            Text->Shape(node._text, {faces, node._fontSize}, node._shaped);
            for (const UIFontFace face : faces)
            {
                ready = ready && Text->IsReady(face);
            }
        }
        node._shapeValid = ready;
        if (!ready && Window != nullptr)
        {
            Window->OnShapedBeforeReady(&node);
        }
    }
    return {node._shaped.Width, node._shaped.Ascent + node._shaped.Descent};
}

Vector2 UILayout::MeasureContent(UIElementNode &node, Vector2 inner) const
{
    if (node._kind == UIElementKind::Text)
    {
        return MeasureText(node);
    }
    const UIStyle &style = node.Style;
    if (style.Layout == UILayoutKind::Grid)
    {
        GridLayout grid;
        SolveGrid(node, inner, grid);
        Vector2 size;
        for (const float32 column : grid.Columns)
        {
            size.X += column;
        }
        for (const float32 row : grid.Rows)
        {
            size.Y += row;
        }
        size.X += style.Gap * static_cast<float32>(grid.Columns.Count() > 0 ? grid.Columns.Count() - 1 : 0);
        size.Y += style.Gap * static_cast<float32>(grid.Rows.Count() > 0 ? grid.Rows.Count() - 1 : 0);
        return size;
    }
    List<UIElementNode *> items;
    CollectItems(node, items, inner);
    const bool row = style.Direction == UIDirection::Row;
    float32 main = 0;
    float32 cross = 0;
    for (UIElementNode *item : items)
    {
        const UIEdges &margin = item->Style.Margin;
        const Vector2 size = Measure(*item, {Shrunk(inner.X, margin.Horizontal()), Shrunk(inner.Y, margin.Vertical())});
        const float32 basis = Resolve(item->Style.Basis, row ? inner.X : inner.Y);
        main += (basis >= 0 ? basis : (row ? size.X : size.Y)) + (row ? margin.Horizontal() : margin.Vertical());
        cross = Max(cross, (row ? size.Y + margin.Vertical() : size.X + margin.Horizontal()));
    }
    if (items.Count() > 1)
    {
        main += style.Gap * static_cast<float32>(items.Count() - 1);
    }
    return row ? Vector2(main, cross) : Vector2(cross, main);
}

void UILayout::Arrange(UIElementNode &node, const UIRect &rect) const
{
    const bool resized = rect.Width != node._bounds.Width || rect.Height != node._bounds.Height;
    node._bounds = rect;
    if (!node._layoutDirty && !resized)
    {
        return;
    }
    node._layoutDirty = false;
    if (node._kind == UIElementKind::Text)
    {
        (void)MeasureText(node);
        return;
    }
    const UIEdges &padding = node.Style.Padding;
    const UIRect inner(padding.Left, padding.Top, Max(rect.Width - padding.Horizontal(), 0.0f),
                       Max(rect.Height - padding.Vertical(), 0.0f));
    if (node.Style.Layout == UILayoutKind::Grid)
    {
        ArrangeGrid(node, inner);
    }
    else
    {
        ArrangeFlex(node, inner);
    }
    List<UIElementNode *> absolute;
    CollectItems(node, absolute, {rect.Width, rect.Height}, true);
    for (UIElementNode *child : absolute)
    {
        const UIStyle &style = child->Style;
        const Vector2 size = Measure(
            *child, {Shrunk(inner.Width, style.Margin.Horizontal()), Shrunk(inner.Height, style.Margin.Vertical())});
        Arrange(*child, {inner.X + style.Offset.X + style.Margin.Left, inner.Y + style.Offset.Y + style.Margin.Top,
                         size.X, size.Y});
    }
}

void UILayout::ArrangeFlex(UIElementNode &node, const UIRect &inner) const
{
    const UIStyle &style = node.Style;
    List<UIElementNode *> nodes;
    CollectItems(node, nodes, {node._bounds.Width, node._bounds.Height});
    if (nodes.IsEmpty())
    {
        return;
    }
    const bool row = style.Direction == UIDirection::Row;
    const float32 innerMain = row ? inner.Width : inner.Height;
    const float32 innerCross = row ? inner.Height : inner.Width;
    List<FlexItem> items;
    items.Reserve(nodes.Count());
    float32 used = style.Gap * static_cast<float32>(nodes.Count() - 1);
    float32 totalGrow = 0;
    float32 totalShrink = 0;
    for (UIElementNode *child : nodes)
    {
        const UIEdges &margin = child->Style.Margin;
        FlexItem item;
        item.Node = child;
        item.MarginMainStart = row ? margin.Left : margin.Top;
        item.MarginMainEnd = row ? margin.Right : margin.Bottom;
        item.MarginCrossStart = row ? margin.Top : margin.Left;
        item.MarginCrossEnd = row ? margin.Bottom : margin.Right;
        const float32 marginMain = item.MarginMainStart + item.MarginMainEnd;
        const float32 marginCross = item.MarginCrossStart + item.MarginCrossEnd;
        float32 base = Resolve(child->Style.Basis, innerMain);
        if (base < 0)
        {
            const Vector2 offered = row ? Vector2(Shrunk(innerMain, marginMain), Shrunk(innerCross, marginCross))
                                        : Vector2(Shrunk(innerCross, marginCross), Shrunk(innerMain, marginMain));
            const Vector2 size = Measure(*child, offered);
            base = row ? size.X : size.Y;
        }
        item.Base = base;
        used += base + marginMain;
        totalGrow += child->Style.Grow;
        totalShrink += child->Style.Shrink * base;
        items.Add(item);
    }
    const float32 free = innerMain - used;
    float32 remaining = innerMain - style.Gap * static_cast<float32>(items.Count() - 1);
    for (FlexItem &item : items)
    {
        const UIStyle &child = item.Node->Style;
        float32 main = item.Base;
        if (free > 0 && totalGrow > 0)
        {
            main += free * child.Grow / totalGrow;
        }
        else if (free < 0 && totalShrink > 0)
        {
            main += free * child.Shrink * item.Base / totalShrink;
        }
        item.Main = row ? Clamp(main, child.MinWidth, child.MaxWidth) : Clamp(main, child.MinHeight, child.MaxHeight);
        remaining -= item.Main + item.MarginMainStart + item.MarginMainEnd;
    }
    float32 cursor = 0;
    float32 between = style.Gap;
    if (remaining > 0)
    {
        const auto count = static_cast<float32>(items.Count());
        switch (style.Justify)
        {
        case UIJustify::Start:
            break;
        case UIJustify::Center:
            cursor = remaining * 0.5f;
            break;
        case UIJustify::End:
            cursor = remaining;
            break;
        case UIJustify::SpaceBetween:
            between += items.Count() > 1 ? remaining / (count - 1) : 0;
            break;
        case UIJustify::SpaceAround:
            cursor = remaining / count * 0.5f;
            between += remaining / count;
            break;
        case UIJustify::SpaceEvenly:
            cursor = remaining / (count + 1);
            between += remaining / (count + 1);
            break;
        }
    }
    for (const FlexItem &item : items)
    {
        UIElementNode &child = *item.Node;
        const UIAlign align = child.Style.AlignSelf != UIAlign::Auto ? child.Style.AlignSelf : style.AlignItems;
        const float32 crossSpace = Max(innerCross - item.MarginCrossStart - item.MarginCrossEnd, 0.0f);
        float32 cross = Resolve(row ? child.Style.Height : child.Style.Width, innerCross);
        if (cross < 0)
        {
            if (align == UIAlign::Stretch)
            {
                cross = crossSpace;
            }
            else
            {
                const Vector2 size =
                    Measure(child, row ? Vector2(item.Main, crossSpace) : Vector2(crossSpace, item.Main));
                cross = row ? size.Y : size.X;
            }
        }
        cross = row ? Clamp(cross, child.Style.MinHeight, child.Style.MaxHeight)
                    : Clamp(cross, child.Style.MinWidth, child.Style.MaxWidth);
        float32 crossPosition = item.MarginCrossStart;
        if (align == UIAlign::Center)
        {
            crossPosition += (crossSpace - cross) * 0.5f;
        }
        else if (align == UIAlign::End)
        {
            crossPosition += crossSpace - cross;
        }
        cursor += item.MarginMainStart;
        const UIRect rect = row ? UIRect(inner.X + cursor, inner.Y + crossPosition, item.Main, cross)
                                : UIRect(inner.X + crossPosition, inner.Y + cursor, cross, item.Main);
        Arrange(child, rect);
        cursor += item.Main + item.MarginMainEnd + between;
    }
}

void UILayout::SolveGrid(UIElementNode &node, Vector2 inner, GridLayout &grid) const
{
    const UIStyle &style = node.Style;
    List<UIElementNode *> nodes;
    CollectItems(node, nodes, inner);
    const bool autoFill = style.AutoFillColumnWidth > 0;
    uint32 columns = style.Columns.IsEmpty() ? 1 : static_cast<uint32>(style.Columns.Count());
    if (autoFill)
    {
        columns = inner.X < Unbounded
                      ? Max(static_cast<uint32>((inner.X + style.Gap) / (style.AutoFillColumnWidth + style.Gap)), 1u)
                      : 1u;
    }
    List<bool> occupied;
    auto cell = [&](uint32 column, uint32 row) -> bool {
        const usize index = static_cast<usize>(row) * columns + column;
        return index < occupied.Count() && occupied[index];
    };
    auto take = [&](uint32 column, uint32 row, uint32 columnSpan, uint32 rowSpan) {
        const usize needed = static_cast<usize>(row + rowSpan) * columns;
        while (occupied.Count() < needed)
        {
            occupied.Add(false);
        }
        for (uint32 r = row; r < row + rowSpan; ++r)
        {
            for (uint32 c = column; c < column + columnSpan; ++c)
            {
                occupied[static_cast<usize>(r) * columns + c] = true;
            }
        }
    };
    auto fits = [&](uint32 column, uint32 row, uint32 columnSpan, uint32 rowSpan) {
        if (column + columnSpan > columns)
        {
            return false;
        }
        for (uint32 r = row; r < row + rowSpan; ++r)
        {
            for (uint32 c = column; c < column + columnSpan; ++c)
            {
                if (cell(c, r))
                {
                    return false;
                }
            }
        }
        return true;
    };
    uint32 rowCount = 0;
    uint32 cursor = 0;
    for (UIElementNode *child : nodes)
    {
        GridPlacement placement;
        placement.Node = child;
        placement.ColumnSpan = Min<uint32>(Max<uint32>(child->Style.ColumnSpan, 1), columns);
        placement.RowSpan = Max<uint32>(child->Style.RowSpan, 1);
        if (child->Style.Column > 0 && child->Style.Row > 0)
        {
            placement.Column = Min<uint32>(child->Style.Column - 1u, columns - placement.ColumnSpan);
            placement.Row = child->Style.Row - 1u;
        }
        else
        {
            const uint32 fixedColumn = child->Style.Column > 0 ? child->Style.Column - 1u : ~0u;
            uint32 position = fixedColumn != ~0u ? 0 : cursor;
            while (true)
            {
                const uint32 column = fixedColumn != ~0u ? fixedColumn : position % columns;
                const uint32 row = fixedColumn != ~0u ? position : position / columns;
                if (fits(column, row, placement.ColumnSpan, placement.RowSpan))
                {
                    placement.Column = column;
                    placement.Row = row;
                    break;
                }
                ++position;
            }
            if (fixedColumn == ~0u)
            {
                cursor = placement.Row * columns + placement.Column + placement.ColumnSpan;
            }
        }
        take(placement.Column, placement.Row, placement.ColumnSpan, placement.RowSpan);
        rowCount = Max(rowCount, placement.Row + placement.RowSpan);
        grid.Items.Add(placement);
    }

    auto solve = [&](List<float32> &sizes, uint32 count, float32 space, bool columnsAxis) {
        sizes.Clear();
        sizes.Resize(count);
        float32 fixed = style.Gap * static_cast<float32>(count > 0 ? count - 1 : 0);
        float32 fractions = 0;
        // Each track's kind first, then one pass over the items sizes the auto tracks: O(tracks + items).
        enum class Sizing : uint8
        {
            Fixed,
            Fraction,
            Auto,
        };
        List<Sizing> sizing;
        sizing.Resize(count);
        for (uint32 index = 0; index < count; ++index)
        {
            const List<UIGridTrack> &tracks = columnsAxis ? style.Columns : style.Rows;
            UIGridTrack track = index < tracks.Count() ? tracks[index] : UIGridTrack::Auto();
            if (columnsAxis && autoFill)
            {
                track = space < Unbounded ? UIGridTrack::Fraction(1) : UIGridTrack::Points(style.AutoFillColumnWidth);
            }
            if (track.Kind == UIGridTrack::Unit::Points)
            {
                sizes[index] = track.Value;
                sizing[index] = Sizing::Fixed;
            }
            else if (track.Kind == UIGridTrack::Unit::Fraction && space < Unbounded)
            {
                fractions += track.Value;
                sizing[index] = Sizing::Fraction;
            }
            else
            {
                sizes[index] = 0;
                sizing[index] = Sizing::Auto;
            }
        }
        for (const GridPlacement &item : grid.Items)
        {
            const uint32 index = columnsAxis ? item.Column : item.Row;
            if ((columnsAxis ? item.ColumnSpan : item.RowSpan) != 1 || index >= count || sizing[index] != Sizing::Auto)
            {
                continue;
            }
            const UIEdges &margin = item.Node->Style.Margin;
            Vector2 offered(Unbounded, Unbounded);
            if (!columnsAxis)
            {
                float32 width = style.Gap * static_cast<float32>(item.ColumnSpan - 1);
                for (uint32 c = item.Column; c < item.Column + item.ColumnSpan; ++c)
                {
                    width += grid.Columns[c];
                }
                offered.X = Shrunk(width, margin.Horizontal());
            }
            const Vector2 measured = Measure(*item.Node, offered);
            sizes[index] =
                Max(sizes[index], columnsAxis ? measured.X + margin.Horizontal() : measured.Y + margin.Vertical());
        }
        for (uint32 index = 0; index < count; ++index)
        {
            if (sizing[index] != Sizing::Fraction)
            {
                fixed += sizes[index];
            }
        }
        if (fractions > 0)
        {
            const float32 share = Max(space - fixed, 0.0f) / fractions;
            for (uint32 index = 0; index < count; ++index)
            {
                const List<UIGridTrack> &tracks = columnsAxis ? style.Columns : style.Rows;
                if ((columnsAxis && autoFill) ||
                    (index < tracks.Count() && tracks[index].Kind == UIGridTrack::Unit::Fraction))
                {
                    sizes[index] = (columnsAxis && autoFill ? 1.0f : tracks[index].Value) * share;
                }
            }
        }
    };
    solve(grid.Columns, columns, inner.X, true);
    solve(grid.Rows, Max(rowCount, static_cast<uint32>(style.Rows.Count())), inner.Y, false);
}

void UILayout::ArrangeGrid(UIElementNode &node, const UIRect &inner) const
{
    GridLayout grid;
    SolveGrid(node, {inner.Width, inner.Height}, grid);
    const float32 gap = node.Style.Gap;
    List<float32> columnStarts;
    List<float32> rowStarts;
    float32 position = 0;
    for (const float32 column : grid.Columns)
    {
        columnStarts.Add(position);
        position += column + gap;
    }
    position = 0;
    for (const float32 row : grid.Rows)
    {
        rowStarts.Add(position);
        position += row + gap;
    }
    for (const GridPlacement &item : grid.Items)
    {
        const uint32 lastColumn = item.Column + item.ColumnSpan - 1;
        const uint32 lastRow = item.Row + item.RowSpan - 1;
        const UIRect area(inner.X + columnStarts[item.Column], inner.Y + rowStarts[item.Row],
                          columnStarts[lastColumn] + grid.Columns[lastColumn] - columnStarts[item.Column],
                          rowStarts[lastRow] + grid.Rows[lastRow] - rowStarts[item.Row]);
        const UIStyle &child = item.Node->Style;
        const UIRect space(area.X + child.Margin.Left, area.Y + child.Margin.Top,
                           Max(area.Width - child.Margin.Horizontal(), 0.0f),
                           Max(area.Height - child.Margin.Vertical(), 0.0f));
        const float32 width = Resolve(child.Width, space.Width);
        const float32 height = Resolve(child.Height, space.Height);
        Arrange(*item.Node, {space.X, space.Y, width >= 0 ? width : space.Width, height >= 0 ? height : space.Height});
    }
}
} // namespace Marlin
