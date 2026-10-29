#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorShell/Dock.h"
#include "EditorUI/Signal.h"
#include "EditorUI/UIElement.h"
#include "Math/Vector.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
class DockingSubsystem;
class EditingContext;
class EditorUISubsystem;
class UIWindowRoot;
struct DockArea;
struct DockInstance;
struct DockNode;
struct WindowHandle;

/// Where a dock goes relative to another: into its stack as a tab, or beside it.
enum class DockZone : uint8
{
    Center,
    Left,
    Right,
    Top,
    Bottom,
};

/// The docks of one editing context (Documentation/Architecture/09 §5): a main area under the context's tab and any
/// number of floating windows, each a tree of splits whose leaves are stacks of docks shown as tabs. The main area
/// may start with a toolbar, which stays on top: it cannot be moved or closed, but docks can be put to its right.
/// The user drags a dock's tab to split, stack or float it, drags the gaps between docks to resize them, and closes
/// docks with ✕ or reopens them from the Window menu. The layout is saved per context type, as DockLayoutState, in
/// `/User/Editor/Layouts/<name>.mtext`.
///
/// A context makes one in its constructor, with its owner current, and returns BuildMainArea from BuildContent.
/// Main thread only.
class EDITORSHELL_API DockSpace
{
public:
    /// Lays out *context*'s docks as saved under *layoutName*, or as *defaultLayout*. *docking* finds the dock types;
    /// null takes the editor's DockingSubsystem. Without an editor UI there are no floating windows.
    DockSpace(EditingContext &context, StringView layoutName, DockLayoutState defaultLayout, bool toolbar = true,
              DockingSubsystem *docking = nullptr);
    ~DockSpace();
    MARLIN_NONCOPYABLE(DockSpace);

    /// The main area, for the context's BuildContent.
    [[nodiscard]] UIElement BuildMainArea();

    /// Fills the toolbar with what *build* returns, such as a row of ToolbarButtons; built at once, under the toolbar's
    /// owner, replacing what was there.
    void SetToolbar(Function<UIElement()> build);

    [[nodiscard]] EditingContext &GetContext() const
    {
        return *_context;
    }

    [[nodiscard]] bool IsOpen(StringView dock) const;
    /// Shows dock *type*, opening it at the right of the main area if it is closed. False if it does not belong in
    /// the context.
    bool Open(const DockType &type);
    /// Closes *dock*; the toolbar never closes.
    bool Close(StringView dock);
    /// Moves *dock* into the stack of *target* (Center) or beside it, or, with an empty *target*, to that edge of the
    /// main area below the toolbar (Center: into an empty main area). Only Right of the toolbar, and Left, Right and
    /// Center of the docks beside it, are allowed. False if not allowed or nothing would change.
    bool MoveDock(StringView dock, StringView target, DockZone zone);

    /// The layout now, as it is saved.
    [[nodiscard]] DockLayoutState Capture() const;
    /// Replaces the layout with *state*; docks it names that do not exist or belong elsewhere are left out. False,
    /// keeping the default layout, if it cannot be used.
    bool Restore(const DockLayoutState &state);
    /// Back to the context's default layout.
    void ResetLayout();

    /// Whether *window* is one of its floating windows.
    [[nodiscard]] bool OwnsWindow(WindowHandle window) const;
    /// The OS moved or resized *window* this frame; a floating window it moves with the button down is the drag of
    /// the dock it holds. Called by DockingSubsystem before Tick.
    void OnWindowEvents(WindowHandle window, bool moved, bool resized);
    /// The layout changed in a way the next Tick saves.
    void MarkDirty();
    /// Frees what was taken out of the layout last frame and saves the layout a moment after it last changed. Called
    /// by DockingSubsystem each frame, before the UI's signal propagation.
    void Tick(float64 now);

private:
    struct DragState;
    struct DropTarget;
    /// What was taken out of the layout in one frame.
    struct Graveyard;

    // Docks and nodes.
    DockInstance *OpenInstance(StringView id);
    void CloseInstance(DockInstance *dock);
    [[nodiscard]] DockInstance *FindInstance(StringView id) const;
    [[nodiscard]] DockNode *StackOf(const DockInstance *dock) const;
    [[nodiscard]] DockArea *AreaOf(const DockNode *node) const;
    [[nodiscard]] UniquePtr<DockNode> &SlotOf(DockArea &area, DockNode *node);
    [[nodiscard]] static UniquePtr<DockNode> MakeStack(DockInstance *dock);
    /// The tree *nodes* describe, without the docks that cannot open; *valid* turns false if they are not a tree.
    UniquePtr<DockNode> BuildTree(const List<DockNodeState> &nodes, bool &valid);
    /// Drops empty stacks and splits, and splits of one, from the tree in *slot*.
    void Normalize(UniquePtr<DockNode> &slot);
    void Bury(UniquePtr<DockNode> &slot);
    void RemoveNode(DockArea &area, DockNode *node);
    void InsertBeside(DockArea &area, DockNode *target, UniquePtr<DockNode> node, DockZone zone, float32 share);
    void InsertAtEdge(DockArea &area, UniquePtr<DockNode> node, DockZone zone, float32 share);
    /// Takes *dock* out of its stack, removing the stack when it empties; false if it was in no stack.
    bool Detach(DockInstance *dock);
    [[nodiscard]] static bool IsToolbar(const DockNode *node);
    [[nodiscard]] bool IsInTop(const DockArea &area, const DockNode *node) const;
    [[nodiscard]] bool IsAllowed(const DockInstance *dock, const DropTarget &target) const;
    void Drop(DockInstance *dock, const DropTarget &target);
    void DropEmptyAreas();
    void CloseAll();
    bool ApplyState(const DockLayoutState &state);
    void Changed();
    [[nodiscard]] bool IsActive() const;

    // Floating windows.
    [[nodiscard]] UIWindowRoot *GetWindowOf(const DockArea &area) const;
    /// A floating window at *x*, *y* (client area, physical pixels) holding *body*, which it takes; null, leaving
    /// *body*, if no window can be opened.
    DockArea *OpenFloating(UniquePtr<DockNode> &body, int32 x, int32 y, int32 width, int32 height);
    /// Closes *area*'s window and the docks in it.
    void CloseArea(DockArea *area);
    void DestroyArea(DockArea *area);
    void CloseWindows();
    void FloatAt(DockInstance *dock, int32 x, int32 y, int32 width, int32 height);
    /// Floats *dock* where it is (⧉).
    void FloatInPlace(DockInstance *dock);
    void UpdateWindowVisibility();

    // UI.
    [[nodiscard]] UIElement BuildAreaHost(DockArea *area);
    [[nodiscard]] UIElement BuildAreaLayout(DockArea *area);
    [[nodiscard]] UIElement BuildNode(DockNode *node);
    [[nodiscard]] UIElement BuildSplitter(DockNode *node, usize index);
    [[nodiscard]] UIElement BuildStack(DockNode *node);
    [[nodiscard]] UIElement BuildToolbar(DockNode *node);
    /// The tabs of a stack of several docks, which drag along the row to reorder.
    [[nodiscard]] UIElement BuildTabStrip(DockNode *node);
    /// Not *interactive*: the header is a window's title bar, so the tab leaves presses to the OS.
    [[nodiscard]] UIElement BuildTab(DockNode *node, DockInstance *dock, bool interactive);
    [[nodiscard]] UIElement BuildDropOverlay(DockArea *area);
    [[nodiscard]] UIElement BuildFloatingWindow(DockArea *area);

    // Dragging a dock by its header: guides show in the window under the cursor and it drops on the one released
    // over. A dock alone in a floating window takes the window along.
    /// The header of *dock* was pressed at *position* in its window; on *tab*, its tab in a row of several.
    void BeginDrag(DockInstance *dock, Vector2 position, const UIElementNode *tab = nullptr);
    /// Moves the dragged tab along its row to the cursor at *x*, *y*; false once the cursor leaves the header, from
    /// when the dock itself is dragged.
    bool ReorderTabs(int32 x, int32 y);
    void StopReorder();
    /// Follows the cursor and the button each frame.
    void TickDrag();
    /// With *drop*, docks it on the guide under the cursor, or floats it there when that is outside the space.
    void EndDrag(bool drop);
    /// The drop under *x*, *y* (physical pixels on the desktop) for the dragged dock, if any; *overSpace* tells
    /// whether the point is over one of the space's windows at all.
    bool FindDropTarget(int32 x, int32 y, DropTarget &target, bool &overSpace);
    /// Lays out the guides for *area* around *point* and finds the one under it.
    bool FindAreaTarget(DockArea &area, Vector2 point, DropTarget &target);

    // Saving.
    [[nodiscard]] String GetLayoutPath() const;
    bool ReadLayout(DockLayoutState &state) const;
    void Save();

    EditingContext *_context = nullptr;
    DockingSubsystem *_docking = nullptr;
    EditorUISubsystem *_ui = nullptr;
    String _layoutName;
    DockLayoutState _default;
    bool _toolbar = true;
    /// What SetToolbar built.
    ReactiveOwner *_toolbarOwner = nullptr;
    /// Whether the layout being built has taken the toolbar's dock yet.
    bool _toolbarPlaced = false;

    ReactiveOwner *_owner = nullptr;
    /// The open docks; the toolbar's is first when there is one.
    List<UniquePtr<DockInstance>> _docks;
    /// The main area first.
    List<UniquePtr<DockArea>> _areas;
    /// Freed two Ticks after they are taken out, since effects built on them may run in the propagation between.
    UniquePtr<Graveyard> _graveyards[2];
    usize _graveyard = 0;
    /// Floating windows whose close button or OS close was pressed, closed next Tick.
    List<DockArea *> _closeRequests;
    /// Windows of destroyed areas, torn down next Tick: their trees may be dispatching the event that closed them.
    List<UIWindowRoot *> _closingWindows;

    /// Splits, stacks and areas changed: the areas build their layout again.
    Signal<uint32> _structure;
    /// A stack shows another tab.
    Signal<uint32> _selection;
    /// A split's shares changed.
    Signal<uint32> _sizes;
    /// A dock is being dragged: the drop overlays show.
    Signal<bool> _dragging;
    UniquePtr<DragState> _drag;

    bool _dirty = false;
    /// When Tick first saw it dirty; negative until then.
    float64 _changedAt = -1;
};
} // namespace Marlin
