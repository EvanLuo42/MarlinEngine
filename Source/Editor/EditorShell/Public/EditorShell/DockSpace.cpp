#include "EditorShell/DockSpace.h"

#include "Application/PlatformApplication.h"
#include "Application/PlatformWindow.h"
#include "Containers/HashMap.h"
#include "Diagnostics/Log.h"
#include "EditorShell/DockingSubsystem.h"
#include "EditorShell/EditingContext.h"
#include "EditorShell/EditingContextSubsystem.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/EditorUISubsystem.h"
#include "EditorUI/UIAnimation.h"
#include "EditorUI/UIWidgets.h"
#include "EditorUI/UIWindowRoot.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Math/MathUtility.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "UIPainter/UIPainter.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
/// One open dock: its content lives under its own owner from when it opens until it closes, wherever it is moved.
struct DockInstance
{
    /// Null for the toolbar.
    const DockType *Type = nullptr;
    /// The name layouts know it by.
    String Id;
    String Title;
    ReactiveOwner *Owner = nullptr;
    UIElementNode *Content = nullptr;
};

/// A stack's row of tabs as built: kept by dock, so reordering moves them rather than building them again; they slide
/// to new places (a FLIP animation, as the main window's tabs do) and the one being dragged follows the pointer.
struct DockTabStrip
{
    Signal<List<DockInstance *>> Order;
    HashMap<DockInstance *, UIElementNode *> Tabs;
    /// A tab moving from where it was drawn to where layout put it: its offset eases from From to zero.
    struct Motion
    {
        bool Placed = false;
        float32 LayoutX = 0;
        float32 From = 0;
        float64 Start = 0;
    };
    HashMap<DockInstance *, Motion> Motions;
    DockInstance *Dragged = nullptr;
    /// The pointer, in the window, and its distance from the dragged tab's left edge.
    float32 PointerX = 0;
    float32 GrabX = 0;
};

/// A split of its children along Axis, or, without children, a stack of docks showing one at a time.
struct DockNode
{
    DockNode *Parent = nullptr;
    bool Split = false;
    DockAxis Axis = DockAxis::Row;
    List<UniquePtr<DockNode>> Children;
    /// The children's shares, relative to each other.
    List<float32> Sizes;
    List<DockInstance *> Docks;
    usize Active = 0;
    /// As last built: drops test the pointer against them, and splitters read their sizes.
    UIElementNode *Element = nullptr;
    UIElementNode *Header = nullptr;
    List<UIElementNode *> ChildElements;
    /// A stack's tabs, while its header is built with more than one.
    DockTabStrip *Strip = nullptr;
};

/// The main area or a floating window: a top row (the main area's toolbar and the docks beside it) and a body.
struct DockArea
{
    UniquePtr<DockNode> Top;
    UniquePtr<DockNode> Body;
    /// A floating window's tree; null for the main area, which is in the main window.
    UIWindowRoot *Window = nullptr;
    bool Shown = false;
    /// As last built.
    UIElementNode *Host = nullptr;
    UIElementNode *TopElement = nullptr;
    UIElementNode *BodyElement = nullptr;
};

struct DockSpace::DropTarget
{
    DockArea *Area = nullptr;
    /// The stack the dock goes into or beside; null for an edge of the area's body.
    DockNode *Node = nullptr;
    DockZone Zone = DockZone::Center;
    /// What the overlay highlights, in the area's window.
    UIRect Preview;
};

struct DockSpace::DragState
{
    DockInstance *Dock = nullptr;
    bool Dragging = false;
    /// Where the header was pressed, on the desktop.
    int32 PressX = 0;
    int32 PressY = 0;
    /// The press relative to the dock's stack, and the stack's size, in its window's painter units.
    Vector2 Grab;
    Vector2 Size;
    float32 SourcePixelsPerUnit = 1;
    /// The floating window the OS is moving, when the dock is all it holds.
    DockArea *Carried = nullptr;
    /// Pressed on a tab of a stack with others: dragging along the tab row reorders the tabs, and only pulling it
    /// out of the header moves the dock. TabGrabX is the press from the tab's left edge.
    bool FromTab = false;
    bool Reordering = false;
    float32 TabGrabX = 0;
    /// A button the dock drops on, where it would go and how it looks.
    struct Guide
    {
        UIRect Rect;
        DropTarget Target;
        /// At an edge of the area rather than in the compass.
        bool Edge = false;
    };
    /// The guides shown in GuideArea, in its window's painter units, and the one under the cursor.
    List<Guide> Guides;
    DockArea *GuideArea = nullptr;
    int32 Hovered = -1;
    /// The cursor in GuideArea's window.
    Vector2 Pointer;
    bool HasTarget = false;
    DropTarget Target;
};

struct DockSpace::Graveyard
{
    List<UniquePtr<DockNode>> Nodes;
    List<UniquePtr<DockInstance>> Docks;
    List<UniquePtr<DockArea>> Areas;
};

namespace
{
/// How far the pointer moves with a tab held before the dock starts dragging, in painter units.
constexpr float32 DockDragThreshold = 6;
/// The smallest a split lets a dock be, in painter units.
constexpr float32 MinDockSize = 48;
/// The share of the body a dock dropped at its edge takes, and of the top row a dock put right of the toolbar.
constexpr float32 EdgeShare = 0.25f;
constexpr float32 ToolbarShare = 0.35f;
/// Floating windows open at least this large, in painter units.
constexpr float32 MinFloatingWidth = 240;
constexpr float32 MinFloatingHeight = 160;
/// How long after the last change the layout is saved, in seconds.
constexpr float64 SaveDelay = 0.5;
constexpr UITransition DockHoverFade{0.08f, &Easing::EaseOutCubic};
/// Tabs sliding to new places in a stack's tab row.
constexpr UITransition DockTabSlide{0.16f, &Easing::EaseOutCubic};

float32 SlideOffset(const DockTabStrip::Motion &motion, float64 now)
{
    const float64 progress = (now - motion.Start) / DockTabSlide.Duration;
    return progress >= 1 ? 0.0f
                         : motion.From * (1 - DockTabSlide.Easing(static_cast<float32>(progress > 0 ? progress : 0)));
}

/// Where *node* is laid out in the window, without the offset it is drawn at.
UIRect LayoutRectOf(const UIElementNode *node)
{
    const Vector2 position = node->GetWindowPosition() - node->Style.Translate;
    return {position.X, position.Y, node->GetBounds().Width, node->GetBounds().Height};
}

UIRect RectOf(const UIElementNode *node)
{
    const Vector2 position = node->GetWindowPosition();
    return {position.X, position.Y, node->GetBounds().Width, node->GetBounds().Height};
}

bool Contains(const UIRect &rect, Vector2 point)
{
    return point.X >= rect.X && point.X < rect.X + rect.Width && point.Y >= rect.Y && point.Y < rect.Y + rect.Height;
}

DockAxis AxisOf(DockZone zone)
{
    return zone == DockZone::Left || zone == DockZone::Right ? DockAxis::Row : DockAxis::Column;
}

bool IsBefore(DockZone zone)
{
    return zone == DockZone::Left || zone == DockZone::Top;
}

/// The part of *rect* that a dock dropped on *zone* takes: *share* of it on that side, or all of it for Center.
UIRect ZoneRect(const UIRect &rect, DockZone zone, float32 share)
{
    switch (zone)
    {
    case DockZone::Left:
        return {rect.X, rect.Y, rect.Width * share, rect.Height};
    case DockZone::Right:
        return {rect.X + rect.Width * (1 - share), rect.Y, rect.Width * share, rect.Height};
    case DockZone::Top:
        return {rect.X, rect.Y, rect.Width, rect.Height * share};
    case DockZone::Bottom:
        return {rect.X, rect.Y + rect.Height * (1 - share), rect.Width, rect.Height * share};
    case DockZone::Center:
        break;
    }
    return rect;
}

usize IndexOfChild(const DockNode *parent, const DockNode *child)
{
    return parent->Children.FindIndex([child](const UniquePtr<DockNode> &entry) { return entry.Get() == child; });
}

/// The stack under *point* in the tree of *node*, or null.
DockNode *StackAt(DockNode *node, Vector2 point)
{
    if (node == nullptr || node->Element == nullptr)
    {
        return nullptr;
    }
    if (!node->Split)
    {
        return Contains(RectOf(node->Element), point) ? node : nullptr;
    }
    for (UniquePtr<DockNode> &child : node->Children)
    {
        if (DockNode *found = StackAt(child.Get(), point))
        {
            return found;
        }
    }
    return nullptr;
}

DockNode *FindStack(DockNode *node, const DockInstance *dock)
{
    if (node == nullptr)
    {
        return nullptr;
    }
    if (!node->Split)
    {
        for (const DockInstance *held : node->Docks)
        {
            if (held == dock)
            {
                return node;
            }
        }
        return nullptr;
    }
    for (UniquePtr<DockNode> &child : node->Children)
    {
        if (DockNode *found = FindStack(child.Get(), dock))
        {
            return found;
        }
    }
    return nullptr;
}

/// Each dock's shadow, under the docks: the area's background is the docks' own color, so the shadows set them apart.
void PaintShadows(UIPainter &painter, const DockNode *node)
{
    if (node == nullptr)
    {
        return;
    }
    if (!node->Split)
    {
        if (node->Element != nullptr)
        {
            painter.DrawBoxShadow(RectOf(node->Element), UICornerRadii(), ThemeMetric("Dock.ShadowBlur")(),
                                  ThemeMetric("Dock.ShadowSpread")(), ThemeColor("Dock.Shadow")());
        }
        return;
    }
    for (const UniquePtr<DockNode> &child : node->Children)
    {
        PaintShadows(painter, child.Get());
    }
}

/// A drop guide: a button holding a small window whose part on *zone*'s side (all of it for Center) is filled. Edge
/// guides fill a thinner strip, as their docks take less of the area.
void PaintGuide(UIPainter &painter, const UIRect &rect, DockZone zone, bool edge, bool hovered)
{
    const float32 radius = ThemeMetric("Radius.Medium")();
    const UICornerRadii radii(radius, radius, radius, radius);
    painter.DrawBoxShadow(rect, radii, 4, 0, ThemeColor("Dock.Shadow")());
    painter.FillRoundedRect(rect, radii, ThemeColor("Dock.GuideBackground")());
    painter.StrokeRoundedRect(rect, radii, 1, ThemeColor(hovered ? "Dock.GuideHoveredBorder" : "Dock.GuideBorder")());
    const float32 inset = rect.Width * 0.24f;
    const UIRect frame{rect.X + inset, rect.Y + inset, rect.Width - inset * 2, rect.Height - inset * 2};
    const UIRect filled = ZoneRect(frame, zone, edge ? 0.3f : 0.5f);
    painter.FillRect(filled, ThemeColor(hovered ? "Dock.GuideHoveredFill" : "Dock.GuideFill")());
    painter.StrokeRoundedRect(frame, UICornerRadii(), 1, ThemeColor("Dock.GuideGlyph")());
}

void CollectDocks(const DockNode *node, List<DockInstance *> &docks)
{
    if (node == nullptr)
    {
        return;
    }
    docks.AddRange(node->Docks.AsSpan());
    for (const UniquePtr<DockNode> &child : node->Children)
    {
        CollectDocks(child.Get(), docks);
    }
}

/// Appends *node* and the nodes under it to *nodes*, parents first.
void Flatten(const DockNode *node, int32 parent, float32 size, List<DockNodeState> &nodes)
{
    if (node == nullptr)
    {
        return;
    }
    DockNodeState &state = nodes.AddDefaulted();
    state.Parent = parent;
    state.Size = size;
    state.Split = node->Split;
    state.Axis = node->Axis;
    state.Active = static_cast<int32>(node->Active);
    for (const DockInstance *dock : node->Docks)
    {
        state.Docks.Add(dock->Id);
    }
    const auto index = static_cast<int32>(nodes.Count() - 1);
    for (usize child = 0; child < node->Children.Count(); ++child)
    {
        Flatten(node->Children[child].Get(), index, node->Sizes[child], nodes);
    }
}

/// The dock *node* shows; null for an empty stack, which only a node taken out of the layout can be.
DockInstance *ShownDock(const DockNode *node)
{
    return node->Docks.IsEmpty() ? nullptr : node->Docks[Min(node->Active, node->Docks.Count() - 1)];
}
} // namespace

DockSpace::DockSpace(EditingContext &context, StringView layoutName, DockLayoutState defaultLayout, bool toolbar,
                     DockingSubsystem *docking)
    : _context(&context), _docking(docking != nullptr ? docking : Editor::GetSubsystem<DockingSubsystem>()),
      _layoutName(layoutName), _default(Move(defaultLayout)), _toolbar(toolbar), _drag(MakeUnique<DragState>())
{
    _graveyards[0] = MakeUnique<Graveyard>();
    _graveyards[1] = MakeUnique<Graveyard>();
    _ui = _docking != nullptr ? _docking->GetUI() : nullptr;
    _owner = Reactive::CreateOwner(Reactive::GetOwner());
    Reactive::RunInOwner(_owner, [this] {
        _structure = CreateSignal<uint32>(0);
        _selection = CreateSignal<uint32>(0);
        _sizes = CreateSignal<uint32>(0);
        _dragging = CreateSignal(false);
    });
    if (_toolbar)
    {
        // Its buttons will come from the context's commands (09 §4.3); for now it is an empty bar.
        UniquePtr<DockInstance> dock = MakeUnique<DockInstance>();
        dock->Id = String(DockLayout::Toolbar);
        dock->Title = "Toolbar";
        dock->Owner = Reactive::CreateOwner(_owner);
        Reactive::RunInOwner(dock->Owner, [&] { dock->Content = Row().Grow(1).GetNode(); });
        _docks.Add(Move(dock));
    }
    _areas.Add(MakeUnique<DockArea>());

    DockLayoutState saved;
    (void)Restore(ReadLayout(saved) ? saved : _default);
    if (_docking != nullptr)
    {
        _docking->Register(this);
    }
    // Floating windows show only while their context is the active tab.
    Reactive::RunInOwner(_owner, [this] {
        CreateEffect([this] {
            (void)IsActive();
            Reactive::Untrack([this] { UpdateWindowVisibility(); });
        });
    });
}

DockSpace::~DockSpace()
{
    if (_dirty)
    {
        Save();
    }
    if (_docking != nullptr)
    {
        _docking->Unregister(this);
    }
    for (usize index = _areas.Count(); index-- > 1;)
    {
        DestroyArea(_areas[index].Get());
    }
    CloseWindows();
    Reactive::DisposeOwner(_owner);
}

// ---- Docks and nodes ----

DockInstance *DockSpace::OpenInstance(StringView id)
{
    if (id == DockLayout::Toolbar)
    {
        if (!_toolbar || _toolbarPlaced)
        {
            return nullptr;
        }
        _toolbarPlaced = true;
        return _docks[0].Get();
    }
    if (FindInstance(id) != nullptr)
    {
        return nullptr;
    }
    const DockType *type = _docking != nullptr ? _docking->FindType(id) : nullptr;
    if (type == nullptr || !type->CanDockIn(*_context))
    {
        return nullptr;
    }
    UniquePtr<DockInstance> dock = MakeUnique<DockInstance>();
    dock->Type = type;
    dock->Id = String(id);
    dock->Title = String(type->GetDisplayName());
    dock->Owner = Reactive::CreateOwner(_owner);
    Reactive::RunInOwner(dock->Owner, [&] { dock->Content = Box({type->Build(*_context)}).Grow(1).GetNode(); });
    DockInstance *opened = dock.Get();
    _docks.Add(Move(dock));
    return opened;
}

void DockSpace::CloseInstance(DockInstance *dock)
{
    const usize index = _docks.FindIndex([dock](const UniquePtr<DockInstance> &entry) { return entry.Get() == dock; });
    if (index == List<UniquePtr<DockInstance>>::NotFound)
    {
        return;
    }
    Reactive::DisposeOwner(dock->Owner);
    dock->Owner = nullptr;
    dock->Content = nullptr;
    _graveyards[_graveyard]->Docks.Add(Move(_docks[index]));
    _docks.RemoveAt(index);
}

DockInstance *DockSpace::FindInstance(StringView id) const
{
    for (const UniquePtr<DockInstance> &dock : _docks)
    {
        if (dock->Id == id)
        {
            return dock.Get();
        }
    }
    return nullptr;
}

DockNode *DockSpace::StackOf(const DockInstance *dock) const
{
    for (const UniquePtr<DockArea> &area : _areas)
    {
        if (DockNode *found = FindStack(area->Top.Get(), dock))
        {
            return found;
        }
        if (DockNode *found = FindStack(area->Body.Get(), dock))
        {
            return found;
        }
    }
    return nullptr;
}

DockArea *DockSpace::AreaOf(const DockNode *node) const
{
    while (node != nullptr && node->Parent != nullptr)
    {
        node = node->Parent;
    }
    for (const UniquePtr<DockArea> &area : _areas)
    {
        if (node != nullptr && (area->Top.Get() == node || area->Body.Get() == node))
        {
            return area.Get();
        }
    }
    return nullptr;
}

UniquePtr<DockNode> &DockSpace::SlotOf(DockArea &area, DockNode *node)
{
    if (node->Parent != nullptr)
    {
        return node->Parent->Children[IndexOfChild(node->Parent, node)];
    }
    return area.Top.Get() == node ? area.Top : area.Body;
}

UniquePtr<DockNode> DockSpace::MakeStack(DockInstance *dock)
{
    UniquePtr<DockNode> stack = MakeUnique<DockNode>();
    stack->Docks.Add(dock);
    return stack;
}

UniquePtr<DockNode> DockSpace::BuildTree(const List<DockNodeState> &nodes, bool &valid)
{
    UniquePtr<DockNode> root;
    List<DockNode *> made;
    for (usize index = 0; index < nodes.Count(); ++index)
    {
        const DockNodeState &state = nodes[index];
        DockNode *parent = nullptr;
        if (index == 0 ? state.Parent != -1
                       : state.Parent < 0 || static_cast<usize>(state.Parent) >= index ||
                             !made[static_cast<usize>(state.Parent)]->Split)
        {
            valid = false;
            break;
        }
        if (index > 0)
        {
            parent = made[static_cast<usize>(state.Parent)];
        }
        UniquePtr<DockNode> node = MakeUnique<DockNode>();
        node->Parent = parent;
        node->Split = state.Split;
        node->Axis = state.Axis;
        if (!state.Split)
        {
            for (const String &id : state.Docks)
            {
                if (DockInstance *dock = OpenInstance(id.AsView()))
                {
                    node->Docks.Add(dock);
                }
            }
            node->Active = static_cast<usize>(Max(state.Active, 0));
            if (node->Active >= node->Docks.Count())
            {
                node->Active = 0;
            }
        }
        made.Add(node.Get());
        if (parent != nullptr)
        {
            parent->Children.Add(Move(node));
            parent->Sizes.Add(Max(state.Size, 0.01f));
        }
        else
        {
            root = Move(node);
        }
    }
    Normalize(root);
    return root;
}

void DockSpace::Normalize(UniquePtr<DockNode> &slot)
{
    DockNode *node = slot.Get();
    if (node == nullptr)
    {
        return;
    }
    if (!node->Split)
    {
        if (node->Docks.IsEmpty())
        {
            Bury(slot);
        }
        return;
    }
    for (usize index = node->Children.Count(); index-- > 0;)
    {
        Normalize(node->Children[index]);
        if (node->Children[index] == nullptr)
        {
            node->Children.RemoveAt(index);
            node->Sizes.RemoveAt(index);
        }
    }
    if (node->Children.IsEmpty())
    {
        Bury(slot);
    }
    else if (node->Children.Count() == 1)
    {
        UniquePtr<DockNode> only = Move(node->Children[0]);
        only->Parent = node->Parent;
        Bury(slot);
        slot = Move(only);
    }
}

void DockSpace::Bury(UniquePtr<DockNode> &slot)
{
    if (slot != nullptr)
    {
        _graveyards[_graveyard]->Nodes.Add(Move(slot));
    }
}

void DockSpace::RemoveNode(DockArea &area, DockNode *node)
{
    DockNode *parent = node->Parent;
    if (parent == nullptr)
    {
        Bury(SlotOf(area, node));
        return;
    }
    const usize index = IndexOfChild(parent, node);
    Bury(parent->Children[index]);
    parent->Children.RemoveAt(index);
    parent->Sizes.RemoveAt(index);
    if (parent->Children.Count() != 1)
    {
        return;
    }
    // A split of one: its child takes its place, joining the split above when that runs the same way.
    UniquePtr<DockNode> only = Move(parent->Children[0]);
    parent->Children.Clear();
    parent->Sizes.Clear();
    DockNode *grandparent = parent->Parent;
    UniquePtr<DockNode> &parentSlot = SlotOf(area, parent);
    if (grandparent != nullptr && only->Split && only->Axis == grandparent->Axis)
    {
        const usize at = IndexOfChild(grandparent, parent);
        const float32 share = grandparent->Sizes[at];
        float32 total = 0;
        for (const float32 size : only->Sizes)
        {
            total += size;
        }
        Bury(parentSlot);
        grandparent->Children.RemoveAt(at);
        grandparent->Sizes.RemoveAt(at);
        for (usize child = 0; child < only->Children.Count(); ++child)
        {
            only->Children[child]->Parent = grandparent;
            grandparent->Children.Insert(at + child, Move(only->Children[child]));
            grandparent->Sizes.Insert(at + child, share * only->Sizes[child] / total);
        }
        only->Children.Clear();
        _graveyards[_graveyard]->Nodes.Add(Move(only));
        return;
    }
    only->Parent = grandparent;
    Bury(parentSlot);
    parentSlot = Move(only);
}

void DockSpace::InsertBeside(DockArea &area, DockNode *target, UniquePtr<DockNode> node, DockZone zone, float32 share)
{
    const DockAxis axis = AxisOf(zone);
    const bool before = IsBefore(zone);
    DockNode *parent = target->Parent;
    if (parent != nullptr && parent->Axis == axis)
    {
        const usize index = IndexOfChild(parent, target);
        const float32 taken = parent->Sizes[index] * share;
        parent->Sizes[index] -= taken;
        const usize at = before ? index : index + 1;
        node->Parent = parent;
        parent->Children.Insert(at, Move(node));
        parent->Sizes.Insert(at, taken);
        return;
    }
    UniquePtr<DockNode> &slot = SlotOf(area, target);
    UniquePtr<DockNode> split = MakeUnique<DockNode>();
    split->Split = true;
    split->Axis = axis;
    split->Parent = parent;
    UniquePtr<DockNode> old = Move(slot);
    old->Parent = split.Get();
    node->Parent = split.Get();
    if (before)
    {
        split->Children.Add(Move(node));
        split->Sizes.Add(share);
    }
    split->Children.Add(Move(old));
    split->Sizes.Add(1 - share);
    if (!before)
    {
        split->Children.Add(Move(node));
        split->Sizes.Add(share);
    }
    slot = Move(split);
}

void DockSpace::InsertAtEdge(DockArea &area, UniquePtr<DockNode> node, DockZone zone, float32 share)
{
    DockNode *root = area.Body.Get();
    if (root == nullptr)
    {
        node->Parent = nullptr;
        area.Body = Move(node);
        return;
    }
    if (root->Split && root->Axis == AxisOf(zone))
    {
        float32 total = 0;
        for (const float32 size : root->Sizes)
        {
            total += size;
        }
        node->Parent = root;
        const usize at = IsBefore(zone) ? 0 : root->Children.Count();
        root->Children.Insert(at, Move(node));
        root->Sizes.Insert(at, total * share / (1 - share));
        return;
    }
    InsertBeside(area, root, Move(node), zone, share);
}

bool DockSpace::Detach(DockInstance *dock)
{
    DockNode *stack = StackOf(dock);
    DockArea *area = stack != nullptr ? AreaOf(stack) : nullptr;
    if (area == nullptr)
    {
        return false;
    }
    const usize index = stack->Docks.IndexOf(dock);
    stack->Docks.RemoveAt(index);
    if (stack->Active > 0 && (index < stack->Active || stack->Active >= stack->Docks.Count()))
    {
        --stack->Active;
    }
    if (stack->Docks.IsEmpty())
    {
        RemoveNode(*area, stack);
    }
    return true;
}

bool DockSpace::IsToolbar(const DockNode *node)
{
    return node != nullptr && !node->Split && node->Docks.Count() == 1 && node->Docks[0]->Type == nullptr;
}

bool DockSpace::IsInTop(const DockArea &area, const DockNode *node) const
{
    while (node != nullptr && node->Parent != nullptr)
    {
        node = node->Parent;
    }
    return node != nullptr && node == area.Top.Get();
}

bool DockSpace::IsAllowed(const DockInstance *dock, const DropTarget &target) const
{
    // The toolbar never moves.
    if (dock == nullptr || dock->Type == nullptr || target.Area == nullptr)
    {
        return false;
    }
    const DockNode *source = StackOf(dock);
    if (source == nullptr)
    {
        return false;
    }
    if (target.Node == nullptr)
    {
        if (target.Area->Body == nullptr)
        {
            return target.Zone == DockZone::Center;
        }
        // Nothing would change when the dock is all the body holds.
        return target.Zone != DockZone::Center && !(target.Area->Body.Get() == source && source->Docks.Count() == 1);
    }
    const DockNode *node = target.Node;
    if (node->Split)
    {
        return false;
    }
    if (IsToolbar(node))
    {
        return target.Zone == DockZone::Right;
    }
    if (IsInTop(*target.Area, node) && (target.Zone == DockZone::Top || target.Zone == DockZone::Bottom))
    {
        return false;
    }
    return node != source || (target.Zone != DockZone::Center && source->Docks.Count() > 1);
}

void DockSpace::Drop(DockInstance *dock, const DropTarget &target)
{
    if (!IsAllowed(dock, target))
    {
        return;
    }
    DockArea &area = *target.Area;
    DockNode *node = target.Node;
    // The target is never the dock's own stack when that empties, so it outlives the detach.
    (void)Detach(dock);
    if (node == nullptr)
    {
        InsertAtEdge(area, MakeStack(dock), target.Zone, EdgeShare);
    }
    else if (target.Zone == DockZone::Center)
    {
        node->Docks.Add(dock);
        node->Active = node->Docks.Count() - 1;
    }
    else
    {
        InsertBeside(area, node, MakeStack(dock), target.Zone, IsToolbar(node) ? ToolbarShare : 0.5f);
    }
    DropEmptyAreas();
    Changed();
    MarkDirty();
}

void DockSpace::DropEmptyAreas()
{
    for (usize index = _areas.Count(); index-- > 1;)
    {
        if (_areas[index]->Body == nullptr)
        {
            DestroyArea(_areas[index].Get());
        }
    }
}

void DockSpace::CloseAll()
{
    for (usize index = _areas.Count(); index-- > 1;)
    {
        DestroyArea(_areas[index].Get());
    }
    DockArea &main = *_areas[0];
    Bury(main.Top);
    Bury(main.Body);
    for (usize index = _docks.Count(); index-- > 0;)
    {
        if (_docks[index]->Type != nullptr)
        {
            CloseInstance(_docks[index].Get());
        }
    }
    _toolbarPlaced = false;
}

void DockSpace::Changed()
{
    _structure.Set(_structure.Peek() + 1);
}

bool DockSpace::IsOpen(StringView dock) const
{
    return FindInstance(dock) != nullptr;
}

bool DockSpace::Open(const DockType &type)
{
    if (_docking == nullptr || !type.CanDockIn(*_context))
    {
        return false;
    }
    const StringView id = _docking->GetName(type);
    if (DockInstance *open = FindInstance(id))
    {
        DockNode *stack = StackOf(open);
        stack->Active = stack->Docks.IndexOf(open);
        _selection.Set(_selection.Peek() + 1);
        const DockArea *area = AreaOf(stack);
        if (area != nullptr && area->Window != nullptr)
        {
            PlatformWindow::Focus(area->Window->GetWindowHandle());
        }
        MarkDirty();
        return true;
    }
    DockInstance *dock = OpenInstance(id);
    if (dock == nullptr)
    {
        return false;
    }
    InsertAtEdge(*_areas[0], MakeStack(dock), DockZone::Right, EdgeShare);
    Changed();
    MarkDirty();
    return true;
}

bool DockSpace::Close(StringView id)
{
    DockInstance *dock = FindInstance(id);
    if (dock == nullptr || dock->Type == nullptr)
    {
        return false;
    }
    (void)Detach(dock);
    CloseInstance(dock);
    DropEmptyAreas();
    Changed();
    MarkDirty();
    return true;
}

bool DockSpace::MoveDock(StringView id, StringView targetId, DockZone zone)
{
    DockInstance *dock = FindInstance(id);
    DropTarget target;
    target.Area = _areas[0].Get();
    target.Zone = zone;
    if (!targetId.IsEmpty())
    {
        const DockInstance *other = FindInstance(targetId);
        target.Node = other != nullptr ? StackOf(other) : nullptr;
        target.Area = target.Node != nullptr ? AreaOf(target.Node) : nullptr;
        if (target.Node == nullptr)
        {
            return false;
        }
    }
    if (!IsAllowed(dock, target))
    {
        return false;
    }
    Drop(dock, target);
    return true;
}

DockLayoutState DockSpace::Capture() const
{
    DockLayoutState state;
    Flatten(_areas[0]->Top.Get(), -1, 1, state.Main.Top);
    Flatten(_areas[0]->Body.Get(), -1, 1, state.Main.Body);
    for (usize index = 1; index < _areas.Count(); ++index)
    {
        const DockArea &area = *_areas[index];
        DockAreaState &floating = state.Floating.AddDefaulted();
        Flatten(area.Body.Get(), -1, 1, floating.Body);
        if (area.Window != nullptr)
        {
            const PlatformRect rect = PlatformWindow::GetClientRect(area.Window->GetWindowHandle());
            floating.X = rect.X;
            floating.Y = rect.Y;
            floating.Width = rect.Width;
            floating.Height = rect.Height;
        }
    }
    return state;
}

bool DockSpace::ApplyState(const DockLayoutState &state)
{
    bool valid = true;
    DockArea &main = *_areas[0];
    main.Top = BuildTree(state.Main.Top, valid);
    main.Body = BuildTree(state.Main.Body, valid);
    const DockNode *top = main.Top.Get();
    if (_toolbar)
    {
        // The top row is the toolbar's stack, or a row of stacks that starts with it.
        bool row = top != nullptr && top->Split && top->Axis == DockAxis::Row && IsToolbar(top->Children[0].Get());
        for (usize index = 0; row && index < top->Children.Count(); ++index)
        {
            row = !top->Children[index]->Split;
        }
        valid = valid && (IsToolbar(top) || row);
    }
    else
    {
        valid = valid && top == nullptr;
    }
    for (usize index = 0; valid && index < state.Floating.Count(); ++index)
    {
        const DockAreaState &floating = state.Floating[index];
        UniquePtr<DockNode> body = BuildTree(floating.Body, valid);
        if (body == nullptr)
        {
            continue;
        }
        if (OpenFloating(body, floating.X, floating.Y, floating.Width, floating.Height) == nullptr)
        {
            // No window to show them in: leave them closed.
            List<DockInstance *> docks;
            CollectDocks(body.Get(), docks);
            Bury(body);
            for (DockInstance *dock : docks)
            {
                CloseInstance(dock);
            }
        }
    }
    return valid;
}

bool DockSpace::Restore(const DockLayoutState &state)
{
    CloseAll();
    const bool restored = ApplyState(state);
    if (!restored)
    {
        CloseAll();
        if (!ApplyState(_default))
        {
            CloseAll();
            if (_toolbar)
            {
                _areas[0]->Top = MakeStack(OpenInstance(DockLayout::Toolbar));
            }
        }
    }
    Changed();
    return restored;
}

void DockSpace::ResetLayout()
{
    (void)Restore(_default);
    MarkDirty();
}

bool DockSpace::OwnsWindow(WindowHandle window) const
{
    for (usize index = 1; index < _areas.Count(); ++index)
    {
        if (_areas[index]->Window != nullptr && _areas[index]->Window->GetWindowHandle() == window)
        {
            return true;
        }
    }
    return false;
}

void DockSpace::MarkDirty()
{
    _dirty = true;
    _changedAt = -1;
}

void DockSpace::Tick(float64 now)
{
    for (DockArea *area : _closeRequests)
    {
        CloseArea(area);
    }
    _closeRequests.Clear();
    CloseWindows();
    // What was buried before the last frame's propagation can go now.
    _graveyard ^= 1;
    _graveyards[_graveyard] = MakeUnique<Graveyard>();
    TickDrag();
    if (_dirty)
    {
        if (_changedAt < 0)
        {
            _changedAt = now;
        }
        else if (now - _changedAt >= SaveDelay)
        {
            Save();
        }
    }
}

bool DockSpace::IsActive() const
{
    const auto *contexts = Editor::GetSubsystem<EditingContextSubsystem>();
    return contexts == nullptr || contexts->GetActive() == _context;
}

// ---- Floating windows ----

UIWindowRoot *DockSpace::GetWindowOf(const DockArea &area) const
{
    if (area.Window != nullptr)
    {
        return area.Window;
    }
    return _ui != nullptr ? _ui->GetMainWindow() : nullptr;
}

DockArea *DockSpace::OpenFloating(UniquePtr<DockNode> &body, int32 x, int32 y, int32 width, int32 height)
{
    if (_ui == nullptr || body == nullptr)
    {
        return nullptr;
    }
    UniquePtr<DockArea> area = MakeUnique<DockArea>();
    DockArea *opened = area.Get();
    WindowDesc desc;
    const String title = _context->Title.Peek();
    desc.Title = title.CStr();
    desc.X = x;
    desc.Y = y;
    desc.Width = Max(width, 160);
    desc.Height = Max(height, 120);
    desc.MinWidth = 200;
    desc.MinHeight = 120;
    desc.Flags = WindowFlags::Resizable | WindowFlags::Hidden;
    if (const UIWindowRoot *main = _ui->GetMainWindow())
    {
        // Stays above the main window and is not in the taskbar.
        desc.Owner = main->GetWindowHandle();
    }
    opened->Window = _ui->OpenWindow(desc, [this, opened] { _closeRequests.Add(opened); });
    if (opened->Window == nullptr)
    {
        return nullptr;
    }
    body->Parent = nullptr;
    opened->Body = Move(body);
    _areas.Add(Move(area));
    opened->Window->SetContent([this, opened] { return BuildFloatingWindow(opened); });
    UpdateWindowVisibility();
    return opened;
}

void DockSpace::CloseArea(DockArea *area)
{
    if (_areas.FindIndex([area](const UniquePtr<DockArea> &entry) { return entry.Get() == area; }) ==
        List<UniquePtr<DockArea>>::NotFound)
    {
        return;
    }
    List<DockInstance *> docks;
    CollectDocks(area->Body.Get(), docks);
    DestroyArea(area);
    for (DockInstance *dock : docks)
    {
        CloseInstance(dock);
    }
    Changed();
    MarkDirty();
}

void DockSpace::DestroyArea(DockArea *area)
{
    const usize index = _areas.FindIndex([area](const UniquePtr<DockArea> &entry) { return entry.Get() == area; });
    if (index == 0 || index == List<UniquePtr<DockArea>>::NotFound)
    {
        return;
    }
    if (area->Window != nullptr)
    {
        // Out of sight now; its tree may be dispatching the event that got here, so it is torn down next Tick.
        PlatformWindow::Hide(area->Window->GetWindowHandle());
        _closingWindows.Add(area->Window);
        area->Window = nullptr;
    }
    _graveyards[_graveyard]->Areas.Add(Move(_areas[index]));
    _areas.RemoveAt(index);
}

void DockSpace::CloseWindows()
{
    for (UIWindowRoot *window : _closingWindows)
    {
        window->SetContent([] { return Box(); });
        if (_ui != nullptr)
        {
            _ui->CloseWindow(window);
        }
    }
    _closingWindows.Clear();
}

void DockSpace::FloatAt(DockInstance *dock, int32 x, int32 y, int32 width, int32 height)
{
    (void)Detach(dock);
    UniquePtr<DockNode> body = MakeStack(dock);
    if (OpenFloating(body, x, y, width, height) == nullptr)
    {
        InsertAtEdge(*_areas[0], Move(body), DockZone::Right, EdgeShare);
    }
    DropEmptyAreas();
    Changed();
    MarkDirty();
}

void DockSpace::FloatInPlace(DockInstance *dock)
{
    DockNode *stack = StackOf(dock);
    DockArea *area = stack != nullptr ? AreaOf(stack) : nullptr;
    const UIWindowRoot *window = area != nullptr ? GetWindowOf(*area) : nullptr;
    if (window == nullptr || stack->Element == nullptr ||
        (area->Window != nullptr && area->Body.Get() == stack && stack->Docks.Count() == 1))
    {
        return;
    }
    // Where it is now, a little down and right so that it shows as a window of its own.
    const WindowHandle handle = window->GetWindowHandle();
    const PlatformRect client = PlatformWindow::GetClientRect(handle);
    const float32 scale = EditorUISubsystem::GetPixelsPerUnit(handle);
    const UIRect rect = RectOf(stack->Element);
    const float32 gap = ThemeMetric("Dock.Gap")();
    const float32 offset = 24;
    FloatAt(dock, client.X + static_cast<int32>((rect.X - gap + offset) * scale),
            client.Y + static_cast<int32>((rect.Y - gap + offset) * scale),
            static_cast<int32>((Max(rect.Width, MinFloatingWidth) + gap * 2) * scale),
            static_cast<int32>((Max(rect.Height, MinFloatingHeight) + gap * 2) * scale));
}

void DockSpace::UpdateWindowVisibility()
{
    const bool active = IsActive();
    for (usize index = 1; index < _areas.Count(); ++index)
    {
        DockArea &area = *_areas[index];
        if (area.Window == nullptr || area.Shown == active)
        {
            continue;
        }
        area.Shown = active;
        if (active)
        {
            PlatformWindow::Show(area.Window->GetWindowHandle());
        }
        else
        {
            PlatformWindow::Hide(area.Window->GetWindowHandle());
        }
    }
}

// ---- UI ----

void DockSpace::SetToolbar(Function<UIElement()> build)
{
    if (!_toolbar)
    {
        return;
    }
    DockInstance *toolbar = _docks[0].Get();
    if (_toolbarOwner != nullptr)
    {
        Reactive::DisposeOwner(_toolbarOwner);
    }
    _toolbarOwner = Reactive::CreateOwner(toolbar->Owner);
    UIElementNode *content = nullptr;
    Reactive::RunInOwner(_toolbarOwner, [&] { content = build().GetNode(); });
    toolbar->Content->SetChildren(content != nullptr ? Span<UIElementNode *const>(&content, 1)
                                                     : Span<UIElementNode *const>());
}

UIElement DockSpace::BuildMainArea()
{
    return Box({BuildAreaHost(_areas[0].Get())})
        .Grow(1)
        .Padding(ThemeMetric("Dock.Gap"))
        .Background(ThemeColor("Dock.AreaBackground"));
}

UIElement DockSpace::BuildAreaHost(DockArea *area)
{
    UIElement host = Box().Grow(1).OnPaint([area](UIPainter &painter, const UIRect &) {
        PaintShadows(painter, area->Top.Get());
        PaintShadows(painter, area->Body.Get());
    });
    UIElementNode *hostNode = host.GetNode();
    area->Host = hostNode;
    UIElementNode *overlay = BuildDropOverlay(area).GetNode();
    // Built again whenever splits, stacks or areas change; the docks' content moves into the new layout.
    CreateEffect([this, area, hostNode, overlay] {
        (void)_structure.Get();
        Reactive::Untrack([&] {
            UIElementNode *children[] = {BuildAreaLayout(area).GetNode(), overlay};
            hostNode->SetChildren(Span<UIElementNode *const>(children, 2));
        });
    });
    Reactive::OnCleanup([area, hostNode] {
        if (area->Host == hostNode)
        {
            area->Host = nullptr;
            area->TopElement = nullptr;
            area->BodyElement = nullptr;
        }
    });
    return host;
}

UIElement DockSpace::BuildAreaLayout(DockArea *area)
{
    UIElement body = area->Body != nullptr ? BuildNode(area->Body.Get()) : Box();
    body.Grow(1);
    area->BodyElement = body.GetNode();
    if (area->Top == nullptr)
    {
        area->TopElement = nullptr;
        return Column({body}).Grow(1);
    }
    UIElement top = BuildNode(area->Top.Get());
    top.Height([] { return UILength(ThemeMetric("Toolbar.HeaderHeight")() + ThemeMetric("Toolbar.Height")() + 2); })
        .Shrink(0);
    area->TopElement = top.GetNode();
    return Column({top, body}).Grow(1).Gap(ThemeMetric("Dock.Gap"));
}

UIElement DockSpace::BuildNode(DockNode *node)
{
    if (!node->Split)
    {
        return BuildStack(node);
    }
    const bool row = node->Axis == DockAxis::Row;
    UIElement split = row ? Row() : Column();
    node->Element = split.GetNode();
    node->Header = nullptr;
    node->ChildElements.Clear();
    for (usize index = 0; index < node->Children.Count(); ++index)
    {
        if (index > 0)
        {
            split.GetNode()->AppendChild(BuildSplitter(node, index - 1).GetNode());
        }
        UIElement child = BuildNode(node->Children[index].Get());
        child.Basis(UILength(0)).Grow([this, node, index] {
            (void)_sizes.Get();
            return index < node->Sizes.Count() ? node->Sizes[index] : 1.0f;
        });
        if (row)
        {
            child.MinWidth(MinDockSize);
        }
        else
        {
            child.MinHeight(MinDockSize);
        }
        node->ChildElements.Add(child.GetNode());
        split.GetNode()->AppendChild(child.GetNode());
    }
    return split;
}

UIElement DockSpace::BuildSplitter(DockNode *node, usize index)
{
    struct Resize
    {
        bool Active = false;
        float32 Start = 0;
        float32 A = 0;
        float32 B = 0;
        float32 WeightA = 0;
        float32 WeightB = 0;
    };
    auto *resize = new Resize;
    Reactive::OnCleanup([resize] { delete resize; });
    const bool row = node->Axis == DockAxis::Row;
    const CursorShape shape = row ? CursorShape::ResizeHorizontal : CursorShape::ResizeVertical;
    const Signal<bool> hovered = CreateSignal(false);
    const Signal<bool> held = CreateSignal(false);
    UIElement splitter = Box().Shrink(0).Background(
        FadeIn([hovered, held] { return hovered.Get() || held.Get(); }, "Dock.SplitterHovered", DockHoverFade));
    if (row)
    {
        splitter.Width(ThemeMetric("Dock.Gap"));
    }
    else
    {
        splitter.Height(ThemeMetric("Dock.Gap"));
    }
    splitter
        .OnPointerEnter([hovered, shape](UIPointerEvent &) {
            hovered.Set(true);
            PlatformApplication::SetCursorShape(shape);
        })
        .OnPointerLeave([hovered, held](UIPointerEvent &) {
            hovered.Set(false);
            if (!held.Peek())
            {
                PlatformApplication::SetCursorShape(CursorShape::Arrow);
            }
        })
        .OnPointerDown([node, index, resize, held, row](UIPointerEvent &event) {
            if (event.Button != MouseButton::Left || index + 1 >= node->ChildElements.Count())
            {
                return;
            }
            event.Handled = true;
            const UIRect &a = node->ChildElements[index]->GetBounds();
            const UIRect &b = node->ChildElements[index + 1]->GetBounds();
            resize->Active = true;
            resize->Start = row ? event.Position.X : event.Position.Y;
            resize->A = row ? a.Width : a.Height;
            resize->B = row ? b.Width : b.Height;
            resize->WeightA = node->Sizes[index];
            resize->WeightB = node->Sizes[index + 1];
            held.Set(true);
        })
        .OnPointerMove([this, node, index, resize, row](UIPointerEvent &event) {
            const float32 total = resize->A + resize->B;
            if (!resize->Active || total <= MinDockSize * 2)
            {
                return;
            }
            const float32 delta = (row ? event.Position.X : event.Position.Y) - resize->Start;
            const float32 a = Math::Clamp(resize->A + delta, MinDockSize, total - MinDockSize);
            const float32 weights = resize->WeightA + resize->WeightB;
            node->Sizes[index] = weights * a / total;
            node->Sizes[index + 1] = weights - node->Sizes[index];
            _sizes.Set(_sizes.Peek() + 1);
        })
        .OnPointerUp([this, resize, hovered, held](UIPointerEvent &) {
            if (!resize->Active)
            {
                return;
            }
            resize->Active = false;
            held.Set(false);
            if (!hovered.Peek())
            {
                PlatformApplication::SetCursorShape(CursorShape::Arrow);
            }
            MarkDirty();
        });
    return splitter;
}

UIElement DockSpace::BuildStack(DockNode *node)
{
    if (IsToolbar(node))
    {
        return BuildToolbar(node);
    }
    // The only dock of a floating window: its header is the window's title bar, which the OS drags (with Snap) and
    // double-clicks to maximize; dragging it still shows the guides (OnWindowEvents).
    const DockArea *area = AreaOf(node);
    const bool lone =
        area != nullptr && area->Window != nullptr && area->Body.Get() == node && node->Docks.Count() == 1;
    UIElement tabs = node->Docks.Count() > 1 ? BuildTabStrip(node) : BuildTab(node, node->Docks[0], !lone);
    // ⧉ floats and ✕ closes the dock the stack shows.
    const auto button = [](StringView icon, Function<void()> action) {
        UIElement element = Box();
        const Signal<bool> hovered = TrackHover(element);
        return WithChildren(
                   Move(element),
                   {IconView(
                       icon, [hovered] { return ThemeColor(hovered.Get() ? "Dock.ButtonHovered" : "Dock.Button")(); },
                       ThemeMetric("Dock.ButtonSize")(), 1.25f)})
            .Width(ThemeMetric("Dock.HeaderHeight"))
            .Shrink(0)
            .Justify(UIJustify::Center)
            .AlignItems(UIAlign::Center)
            .OnPointerDown([](UIPointerEvent &event) { event.Handled = true; })
            .OnClick([action = Move(action)](UIPointerEvent &event) {
                event.Handled = true;
                action();
            });
    };
    UIElement header = Row({tabs});
    header.GetNode()->AppendChild(Box().Grow(1).GetNode());
    if (_ui != nullptr)
    {
        header.GetNode()->AppendChild(button("Copy", [this, node] {
                                          if (DockInstance *dock = ShownDock(node))
                                          {
                                              FloatInPlace(dock);
                                          }
                                      }).GetNode());
    }
    header.GetNode()->AppendChild(button("X", [this, node] {
                                      if (const DockInstance *dock = ShownDock(node))
                                      {
                                          (void)Close(dock->Id.AsView());
                                      }
                                  }).GetNode());
    header.Height(ThemeMetric("Dock.HeaderHeight")).Shrink(0).Background(ThemeColor("Dock.Header"));
    if (lone)
    {
        header.WindowCaption();
    }
    else
    {
        header.OnPointerDown([this, node](UIPointerEvent &event) {
            // Anywhere on the header drags the dock it shows; the tabs and buttons handle their own presses.
            DockInstance *dock = ShownDock(node);
            if (event.Button == MouseButton::Left && dock != nullptr)
            {
                event.Handled = true;
                BeginDrag(dock, event.Position);
            }
        });
    }

    UIElement body = Box().Grow(1).Clip().Background(ThemeColor("Dock.Background"));
    UIElementNode *bodyNode = body.GetNode();
    CreateEffect([this, node, bodyNode] {
        (void)_selection.Get();
        Reactive::Untrack([&] {
            if (const DockInstance *dock = ShownDock(node))
            {
                UIElementNode *content[] = {dock->Content};
                bodyNode->SetChildren(Span<UIElementNode *const>(content, 1));
            }
        });
    });
    UIElement stack = Column({header, body}).Clip();
    node->Element = stack.GetNode();
    node->Header = header.GetNode();
    return stack;
}

UIElement DockSpace::BuildToolbar(DockNode *node)
{
    UIElement header =
        Row({Text("Toolbar").FontSize(ThemeMetric("FontSize.Small")).TextColor(ThemeColor("Toolbar.HeaderText"))})
            .Height(ThemeMetric("Toolbar.HeaderHeight"))
            .Shrink(0)
            .AlignItems(UIAlign::Center)
            .Padding([] { return UIEdges(ThemeMetric("Toolbar.PaddingX")(), 0); })
            .Background(ThemeColor("Toolbar.Header"));
    UIElement body = Box().Grow(1).Clip().Background(ThemeColor("Toolbar.Background"));
    body.GetNode()->AppendChild(node->Docks[0]->Content);
    UIElement stack = Column({header, body}).Clip();
    node->Element = stack.GetNode();
    node->Header = header.GetNode();
    return stack;
}

UIElement DockSpace::BuildTabStrip(DockNode *node)
{
    auto *strip = new DockTabStrip;
    strip->Order = CreateSignal(node->Docks);
    node->Strip = strip;
    Reactive::OnCleanup([node, strip] {
        if (node->Strip == strip)
        {
            node->Strip = nullptr;
        }
        delete strip;
    });
    UIElement row = Row({For<DockInstance *>([strip] { return strip->Order.Get(); },
                                             [](DockInstance *const &dock) { return reinterpret_cast<uint64>(dock); },
                                             [this, node, strip](Signal<DockInstance *> item) {
                                                 DockInstance *dock = item.Peek();
                                                 UIElement tab = BuildTab(node, dock, true);
                                                 strip->Tabs.Set(dock, tab.GetNode());
                                                 Reactive::OnCleanup([strip, dock] {
                                                     strip->Tabs.Remove(dock);
                                                     strip->Motions.Remove(dock);
                                                 });
                                                 return tab;
                                             })})
                        .Shrink(0);
    row.OnPaint([strip](UIPainter &, const UIRect &rect) {
        // Before the tabs paint: start a slide for each tab layout moved, and place the dragged one at the pointer.
        const float64 now = UIAnimation::PeekNow();
        for (const auto &entry : strip->Tabs)
        {
            const float32 x = entry.Value->GetBounds().X;
            DockTabStrip::Motion &motion = strip->Motions.FindOrAdd(entry.Key);
            if (motion.Placed && x != motion.LayoutX)
            {
                motion.From = motion.LayoutX + SlideOffset(motion, now) - x;
                motion.Start = now;
            }
            motion.Placed = true;
            motion.LayoutX = x;
            entry.Value->Style.Translate = Vector2(SlideOffset(motion, now), 0);
        }
        if (UIElementNode *const *dragged = strip->Dragged != nullptr ? strip->Tabs.Find(strip->Dragged) : nullptr)
        {
            const float32 width = (*dragged)->GetBounds().Width;
            const float32 left = Math::Clamp(strip->PointerX - strip->GrabX, rect.X, rect.X + rect.Width - width);
            (*dragged)->Style.Translate = Vector2(left - LayoutRectOf(*dragged).X, 0);
        }
    });
    return row;
}

UIElement DockSpace::BuildTab(DockNode *node, DockInstance *dock, bool interactive)
{
    // A lone dock's tab is just its title, as in the design; stacked docks' tabs show which one is shown.
    const bool single = node->Docks.Count() == 1;
    const auto shown = [this, node, dock] {
        (void)_selection.Get();
        return ShownDock(node) == dock;
    };
    UIElement tab = Box();
    const Signal<bool> hovered = TrackHover(tab);
    tab =
        WithChildren(Move(tab), {Text(dock->Title).FontSize(ThemeMetric("FontSize.Normal")).TextColor([shown, single] {
                         return ThemeColor(single || shown() ? "Dock.TitleText" : "Dock.TabText")();
                     })})
            .Shrink(0)
            .Justify(UIJustify::Center)
            .Padding([] { return UIEdges(ThemeMetric("Dock.TabPaddingX")(), 0); })
            .Radius([] {
                const float32 radius = ThemeMetric("Dock.TabRadius")();
                return UICornerRadii(radius, radius, 0, 0);
            });
    if (!single)
    {
        tab.Background(Animate<LinearColor>(
            [shown, hovered] {
                return ThemeColor(shown() ? "Dock.TabActive" : hovered.Get() ? "Dock.TabHovered" : "Dock.Header")();
            },
            DockHoverFade));
    }
    if (!interactive)
    {
        return tab;
    }
    UIElementNode *tabNode = tab.GetNode();
    tab.OnPointerDown([this, node, dock, tabNode](UIPointerEvent &event) {
        if (event.Button == MouseButton::Middle)
        {
            event.Handled = true;
            (void)Close(dock->Id.AsView());
            return;
        }
        if (event.Button != MouseButton::Left)
        {
            return;
        }
        event.Handled = true;
        const usize index = node->Docks.IndexOf(dock);
        if (index != List<DockInstance *>::NotFound && index != node->Active)
        {
            node->Active = index;
            _selection.Set(_selection.Peek() + 1);
            MarkDirty();
        }
        BeginDrag(dock, event.Position, node->Docks.Count() > 1 ? tabNode : nullptr);
    });
    return tab;
}

UIElement DockSpace::BuildDropOverlay(DockArea *area)
{
    const DragState *drag = _drag.Get();
    // Over the area while a dock is dragged, highlighting where it would go. Painted every frame, so it follows the
    // drag without signals.
    return Box()
        .Absolute(Vector2())
        .Width(UILength::Percent(100))
        .Height(UILength::Percent(100))
        .Visible(_dragging)
        .OnPaint([drag, area](UIPainter &painter, const UIRect &) {
            if (!drag->Dragging || drag->GuideArea != area)
            {
                return;
            }
            if (drag->HasTarget && drag->Target.Area == area)
            {
                const UIRect &rect = drag->Target.Preview;
                painter.FillRect(rect, ThemeColor("Dock.DropPreview")());
                painter.StrokeRoundedRect(rect, UICornerRadii(), 1, ThemeColor("Dock.DropPreviewBorder")());
            }
            else if (drag->Carried == nullptr)
            {
                // Off the guides it would float: outline the window it would get, where the dock was grabbed.
                const UIRect rect{drag->Pointer.X - drag->Grab.X, drag->Pointer.Y - drag->Grab.Y,
                                  Max(drag->Size.X, MinFloatingWidth), Max(drag->Size.Y, MinFloatingHeight)};
                painter.FillRect(rect, ThemeColor("Dock.FloatPreview")());
                painter.StrokeRoundedRect(rect, UICornerRadii(), 1, ThemeColor("Dock.FloatPreviewBorder")());
            }
            for (usize index = 0; index < drag->Guides.Count(); ++index)
            {
                const DragState::Guide &guide = drag->Guides[index];
                PaintGuide(painter, guide.Rect, guide.Target.Zone, guide.Edge,
                           static_cast<int32>(index) == drag->Hovered);
            }
        });
}

UIElement DockSpace::BuildFloatingWindow(DockArea *area)
{
    UIElement close = Box();
    const Signal<bool> hovered = TrackHover(close);
    close = WithChildren(Move(close), {IconView(
                                          "X",
                                          [hovered] {
                                              return ThemeColor(hovered.Get() ? "WindowButton.Close.HoveredGlyph"
                                                                              : "WindowButton.Glyph")();
                                          },
                                          ThemeMetric("FloatingWindow.IconSize")(), 1)})
                .Width(ThemeMetric("FloatingWindow.ButtonWidth"))
                .AlignSelf(UIAlign::Stretch)
                .Justify(UIJustify::Center)
                .AlignItems(UIAlign::Center)
                .Background(FadeIn([hovered] { return hovered.Get(); }, "WindowButton.Close.Hovered", DockHoverFade))
                .OnClick([this, area](UIPointerEvent &) { _closeRequests.Add(area); });
    UIElement title =
        Row({
                Text(_context->Title).FontSize(ThemeMetric("FontSize.Small")).TextColor(ThemeColor("Text.Secondary")),
                Box().Grow(1),
                close,
            })
            .Height(ThemeMetric("FloatingWindow.TitleHeight"))
            .Shrink(0)
            .AlignItems(UIAlign::Center)
            .Padding([] { return UIEdges(ThemeMetric("Toolbar.PaddingX")(), 0, 0, 0); })
            .Background(ThemeColor("TitleBar.Background"))
            .WindowCaption()
            // A lone stack's header moves the window and its close button closes it; several stacks need a bar.
            .Visible([this, area] {
                (void)_structure.Get();
                return area->Body != nullptr && area->Body->Split;
            });
    return Column({title, Box({BuildAreaHost(area)}).Grow(1).Padding(ThemeMetric("Dock.Gap"))})
        .Grow(1)
        .Background(ThemeColor("Dock.AreaBackground"))
        .Border(1, ThemeColor("Window.Border"));
}

// ---- Dragging ----

void DockSpace::BeginDrag(DockInstance *dock, Vector2 position, const UIElementNode *tab)
{
    DockNode *stack = StackOf(dock);
    const DockArea *area = stack != nullptr ? AreaOf(stack) : nullptr;
    const UIWindowRoot *window = area != nullptr ? GetWindowOf(*area) : nullptr;
    DragState &drag = *_drag;
    drag = {};
    if (window == nullptr || stack->Element == nullptr || dock->Type == nullptr)
    {
        return;
    }
    const UIRect rect = RectOf(stack->Element);
    drag.Dock = dock;
    PlatformApplication::GetCursorPosition(drag.PressX, drag.PressY);
    drag.Grab = position - Vector2(rect.X, rect.Y);
    drag.Size = Vector2(rect.Width, rect.Height);
    drag.SourcePixelsPerUnit = EditorUISubsystem::GetPixelsPerUnit(window->GetWindowHandle());
    if (tab != nullptr && stack->Strip != nullptr)
    {
        drag.FromTab = true;
        drag.TabGrabX = position.X - LayoutRectOf(tab).X;
    }
}

void DockSpace::TickDrag()
{
    // Polled each frame rather than driven by the pressed element's events, which stop when the pointer leaves its
    // window and end when the element is rebuilt.
    DragState &drag = *_drag;
    if (drag.Dock == nullptr)
    {
        return;
    }
    const bool open = _docks.FindIndex([&drag](const UniquePtr<DockInstance> &entry) {
        return entry.Get() == drag.Dock;
    }) != List<UniquePtr<DockInstance>>::NotFound;
    const bool carried = drag.Carried == nullptr || _areas.FindIndex([&drag](const UniquePtr<DockArea> &entry) {
        return entry.Get() == drag.Carried;
    }) != List<UniquePtr<DockArea>>::NotFound;
    if (_ui == nullptr || !open || !carried)
    {
        EndDrag(false);
        return;
    }
    if (!PlatformApplication::IsMouseButtonDown(MouseButton::Left))
    {
        EndDrag(true);
        return;
    }
    int32 x = 0;
    int32 y = 0;
    PlatformApplication::GetCursorPosition(x, y);
    if (!drag.Dragging && !drag.Reordering)
    {
        const float32 threshold = DockDragThreshold * drag.SourcePixelsPerUnit;
        if (Math::Abs(static_cast<float32>(x - drag.PressX)) < threshold &&
            Math::Abs(static_cast<float32>(y - drag.PressY)) < threshold)
        {
            return;
        }
        if (drag.FromTab)
        {
            drag.Reordering = true;
        }
        else
        {
            drag.Dragging = true;
            _dragging.Set(true);
        }
    }
    if (drag.Reordering && ReorderTabs(x, y))
    {
        return;
    }
    bool over = false;
    drag.HasTarget = FindDropTarget(x, y, drag.Target, over);
}

void DockSpace::OnWindowEvents(WindowHandle window, bool moved, bool resized)
{
    DockArea *area = nullptr;
    for (usize index = 1; index < _areas.Count(); ++index)
    {
        if (_areas[index]->Window != nullptr && _areas[index]->Window->GetWindowHandle() == window)
        {
            area = _areas[index].Get();
        }
    }
    if (area == nullptr)
    {
        return;
    }
    MarkDirty();
    // The OS moving a window that holds a single dock, with the button down, is that dock being dragged by its
    // header (the window's title bar): show the guides beneath it. A resize from the left or top edge moves the
    // window too, but also resizes it.
    DragState &drag = *_drag;
    DockNode *stack = area->Body.Get();
    if (!moved || resized || drag.Dock != nullptr || stack == nullptr || stack->Split || stack->Docks.Count() != 1 ||
        !PlatformApplication::IsMouseButtonDown(MouseButton::Left))
    {
        return;
    }
    drag = {};
    drag.Dock = stack->Docks[0];
    drag.Carried = area;
    drag.Dragging = true;
    drag.SourcePixelsPerUnit = EditorUISubsystem::GetPixelsPerUnit(window);
    _dragging.Set(true);
}

bool DockSpace::ReorderTabs(int32 x, int32 y)
{
    DragState &drag = *_drag;
    DockNode *node = StackOf(drag.Dock);
    const DockArea *area = node != nullptr ? AreaOf(node) : nullptr;
    const UIWindowRoot *window = area != nullptr ? GetWindowOf(*area) : nullptr;
    DockTabStrip *strip = node != nullptr ? node->Strip : nullptr;
    if (window == nullptr || strip == nullptr || node->Header == nullptr)
    {
        StopReorder();
        return false;
    }
    const WindowHandle handle = window->GetWindowHandle();
    const PlatformRect client = PlatformWindow::GetClientRect(handle);
    const float32 scale = EditorUISubsystem::GetPixelsPerUnit(handle);
    const Vector2 point(static_cast<float32>(x - client.X) / scale, static_cast<float32>(y - client.Y) / scale);
    // Pulled out of the header: from here on the dock moves.
    const UIRect header = RectOf(node->Header);
    const float32 pull = header.Height;
    if (point.Y < header.Y - pull || point.Y > header.Y + header.Height + pull)
    {
        StopReorder();
        drag.Dragging = true;
        _dragging.Set(true);
        return false;
    }
    strip->Dragged = drag.Dock;
    strip->GrabX = drag.TabGrabX;
    strip->PointerX = point.X;
    // Its place is past every other tab whose middle its own middle has passed.
    UIElementNode *const *dragged = strip->Tabs.Find(drag.Dock);
    if (dragged == nullptr)
    {
        return true;
    }
    const float32 middle = point.X - drag.TabGrabX + (*dragged)->GetBounds().Width * 0.5f;
    usize place = 0;
    for (DockInstance *dock : node->Docks)
    {
        UIElementNode *const *tab = strip->Tabs.Find(dock);
        if (dock != drag.Dock && tab != nullptr)
        {
            const UIRect rect = LayoutRectOf(*tab);
            place += middle > rect.X + rect.Width * 0.5f ? 1 : 0;
        }
    }
    const usize index = node->Docks.IndexOf(drag.Dock);
    if (place != index)
    {
        DockInstance *shown = ShownDock(node);
        node->Docks.RemoveAt(index);
        node->Docks.Insert(place, drag.Dock);
        node->Active = node->Docks.IndexOf(shown);
        strip->Order.Set(node->Docks);
        _selection.Set(_selection.Peek() + 1);
        MarkDirty();
    }
    return true;
}

void DockSpace::StopReorder()
{
    DragState &drag = *_drag;
    DockNode *node = drag.Reordering ? StackOf(drag.Dock) : nullptr;
    drag.Reordering = false;
    DockTabStrip *strip = node != nullptr ? node->Strip : nullptr;
    if (strip == nullptr || strip->Dragged == nullptr)
    {
        return;
    }
    // Glides from where it was let go into its place.
    UIElementNode *const *tab = strip->Tabs.Find(strip->Dragged);
    DockTabStrip::Motion *motion = strip->Motions.Find(strip->Dragged);
    if (tab != nullptr && motion != nullptr)
    {
        motion->From = (*tab)->Style.Translate.X;
        motion->Start = UIAnimation::PeekNow();
        motion->LayoutX = (*tab)->GetBounds().X;
    }
    strip->Dragged = nullptr;
}

void DockSpace::EndDrag(bool drop)
{
    DragState &drag = *_drag;
    StopReorder();
    if (drag.Dragging)
    {
        // On a guide it docks there; anywhere else it floats where it was let go, as in Visual Studio. A window the OS
        // was moving with it stays where it was moved.
        int32 x = 0;
        int32 y = 0;
        PlatformApplication::GetCursorPosition(x, y);
        DropTarget target;
        bool over = false;
        if (drop && FindDropTarget(x, y, target, over))
        {
            Drop(drag.Dock, target);
        }
        else if (drop && drag.Carried == nullptr)
        {
            const float32 scale = drag.SourcePixelsPerUnit;
            const float32 gap = ThemeMetric("Dock.Gap")();
            FloatAt(drag.Dock, x - static_cast<int32>((drag.Grab.X + gap) * scale),
                    y - static_cast<int32>((drag.Grab.Y + gap) * scale),
                    static_cast<int32>((Max(drag.Size.X, MinFloatingWidth) + gap * 2) * scale),
                    static_cast<int32>((Max(drag.Size.Y, MinFloatingHeight) + gap * 2) * scale));
        }
        else if (drag.Carried != nullptr)
        {
            MarkDirty();
        }
        _dragging.Set(false);
    }
    drag = {};
}

bool DockSpace::FindDropTarget(int32 x, int32 y, DropTarget &target, bool &overSpace)
{
    overSpace = false;
    DragState &drag = *_drag;
    drag.Guides.Clear();
    drag.GuideArea = nullptr;
    drag.Hovered = -1;
    // A window carried with the dock is under the cursor; what matters is the window beneath it.
    const DockArea *carried = drag.Carried;
    UIWindowRoot *window =
        _ui != nullptr ? _ui->FindWindowAt(x, y, carried != nullptr ? carried->Window : nullptr) : nullptr;
    if (window == nullptr)
    {
        return false;
    }
    for (const UniquePtr<DockArea> &area : _areas)
    {
        if (area.Get() == carried || GetWindowOf(*area) != window || area->Host == nullptr)
        {
            continue;
        }
        overSpace = true;
        const WindowHandle handle = window->GetWindowHandle();
        const PlatformRect client = PlatformWindow::GetClientRect(handle);
        const float32 scale = EditorUISubsystem::GetPixelsPerUnit(handle);
        const Vector2 point(static_cast<float32>(x - client.X) / scale, static_cast<float32>(y - client.Y) / scale);
        return FindAreaTarget(*area, point, target);
    }
    return false;
}

bool DockSpace::FindAreaTarget(DockArea &area, Vector2 point, DropTarget &target)
{
    // Guides, as in Visual Studio: one at the middle of each edge of the body, for a dock along that whole edge, and a
    // compass over the stack under the pointer, for into it or beside it. Only the guide under the pointer drops.
    DragState &drag = *_drag;
    drag.GuideArea = &area;
    drag.Pointer = point;
    const float32 size = ThemeMetric("Dock.GuideSize")();
    const float32 spacing = size + ThemeMetric("Dock.GuideGap")();
    const auto add = [&](Vector2 center, DockNode *node, DockZone zone, const UIRect &preview, bool edge) {
        DragState::Guide guide;
        guide.Rect = {center.X - size * 0.5f, center.Y - size * 0.5f, size, size};
        guide.Edge = edge;
        guide.Target.Area = &area;
        guide.Target.Node = node;
        guide.Target.Zone = zone;
        guide.Target.Preview = preview;
        if (IsAllowed(drag.Dock, guide.Target))
        {
            drag.Guides.Add(guide);
        }
    };
    if (area.BodyElement != nullptr)
    {
        const UIRect body = RectOf(area.BodyElement);
        const Vector2 middle(body.X + body.Width * 0.5f, body.Y + body.Height * 0.5f);
        if (area.Body == nullptr)
        {
            add(middle, nullptr, DockZone::Center, body, true);
        }
        else
        {
            const float32 inset = size * 0.5f + ThemeMetric("Dock.GuideGap")() * 2;
            add({body.X + inset, middle.Y}, nullptr, DockZone::Left, ZoneRect(body, DockZone::Left, EdgeShare), true);
            add({body.X + body.Width - inset, middle.Y}, nullptr, DockZone::Right,
                ZoneRect(body, DockZone::Right, EdgeShare), true);
            add({middle.X, body.Y + inset}, nullptr, DockZone::Top, ZoneRect(body, DockZone::Top, EdgeShare), true);
            add({middle.X, body.Y + body.Height - inset}, nullptr, DockZone::Bottom,
                ZoneRect(body, DockZone::Bottom, EdgeShare), true);
        }
    }
    DockNode *stack = StackAt(area.Top.Get(), point);
    if (stack == nullptr)
    {
        stack = StackAt(area.Body.Get(), point);
    }
    if (stack != nullptr)
    {
        const UIRect rect = RectOf(stack->Element);
        const Vector2 center(rect.X + rect.Width * 0.5f, rect.Y + rect.Height * 0.5f);
        const float32 share = IsToolbar(stack) ? ToolbarShare : 0.5f;
        add(center, stack, DockZone::Center, rect, false);
        add({center.X - spacing, center.Y}, stack, DockZone::Left, ZoneRect(rect, DockZone::Left, share), false);
        add({center.X + spacing, center.Y}, stack, DockZone::Right, ZoneRect(rect, DockZone::Right, share), false);
        add({center.X, center.Y - spacing}, stack, DockZone::Top, ZoneRect(rect, DockZone::Top, share), false);
        add({center.X, center.Y + spacing}, stack, DockZone::Bottom, ZoneRect(rect, DockZone::Bottom, share), false);
    }
    // The compass was added last and wins where it covers an edge guide.
    for (usize index = drag.Guides.Count(); index-- > 0;)
    {
        if (Contains(drag.Guides[index].Rect, point))
        {
            target = drag.Guides[index].Target;
            drag.Hovered = static_cast<int32>(index);
            return true;
        }
    }
    return false;
}

// ---- Saving ----

String DockSpace::GetLayoutPath() const
{
    return Format("/User/Editor/Layouts/{0}.mtext", _layoutName);
}

bool DockSpace::ReadLayout(DockLayoutState &state) const
{
    // Only the editor's own spaces keep layouts; tests and tools without a UI start from the default every time.
    if (_ui == nullptr || _layoutName.IsEmpty() || !Vfs::IsMounted("/User"))
    {
        return false;
    }
    String text;
    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    const MarlinTextObject *object = nullptr;
    return Vfs::ReadAll(GetLayoutPath().AsView(), text) && document.Parse(text.AsView(), diagnostics) &&
           document.GetObjectCount() == 1 && (object = document.GetObject(0, diagnostics)) != nullptr &&
           MarlinTextSerializer::ReadObject(*object, TypeOf<DockLayoutState>(), &state, diagnostics);
}

void DockSpace::Save()
{
    _dirty = false;
    _changedAt = -1;
    if (_ui == nullptr || _layoutName.IsEmpty() || !Vfs::IsMounted("/User"))
    {
        return;
    }
    const DockLayoutState state = Capture();
    StringBuilder<4096> text;
    MarlinTextSerializer::WriteObject(text, TypeOf<DockLayoutState>(), &state);
    text.Append('\n');
    (void)Vfs::CreateDirectory("/User/Editor/Layouts");
    if (!Vfs::WriteAll(GetLayoutPath().AsView(), text.AsView()))
    {
        MARLIN_LOG(LogCore, Warning, "The dock layout cannot be saved to {0}", GetLayoutPath());
    }
}
} // namespace Marlin
