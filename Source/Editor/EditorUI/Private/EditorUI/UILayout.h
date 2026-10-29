#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorUI/UIElement.h"
#include "Math/Vector.h"
#include "UIPainter/UIGeometry.h"

namespace Marlin
{
class UITextSubsystem;
class UIWindowRoot;

/// Flex and grid layout of an element tree (Documentation/Architecture/09 §4.1): Measure asks a node how large it
/// wants to be within *available*, Arrange gives it its rectangle. Both cache per node, so only dirty subtrees are
/// laid out again.
struct UILayout
{
    const UITextSubsystem *Text = nullptr;
    UIWindowRoot *Window = nullptr;

    /// The size *node* wants, margins excluded, given *available* space.
    Vector2 Measure(UIElementNode &node, Vector2 available) const;
    /// Places *node* in *rect* (its margin box) and arranges its children.
    void Arrange(UIElementNode &node, const UIRect &rect) const;
    /// Puts a fragment at its parent's origin, *size* large, so its children are positioned in the parent's space.
    static void PlaceFragment(UIElementNode &fragment, Vector2 size);

private:
    struct GridPlacement
    {
        UIElementNode *Node = nullptr;
        uint32 Column = 0;
        uint32 Row = 0;
        uint32 ColumnSpan = 1;
        uint32 RowSpan = 1;
    };

    struct GridLayout
    {
        List<GridPlacement> Items;
        List<float32> Columns;
        List<float32> Rows;
    };

    Vector2 MeasureContent(UIElementNode &node, Vector2 inner) const;
    Vector2 MeasureText(UIElementNode &node) const;
    void ArrangeFlex(UIElementNode &node, const UIRect &inner) const;
    void ArrangeGrid(UIElementNode &node, const UIRect &inner) const;
    void SolveGrid(UIElementNode &node, Vector2 inner, GridLayout &grid) const;
};
} // namespace Marlin
