#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorUI/UIElement.h"
#include "String/String.h"
#include "String/StringView.h"
#include "EditorShell/Dock.gen.h"

namespace Marlin
{
class EditingContext;

/// A kind of dock: a panel of an editing context, laid out with the others in the context's DockSpace, where the user
/// splits, stacks, moves and floats it (Documentation/Architecture/09 §5). Provider extension point (13 §3): linked
/// types are found by reflection, and a dock is named by its type's name, in layouts too. Stateless.
MCLASS(Abstract, ExtensionPoint)
class EDITORSHELL_API DockType
{
    MGENERATED_BODY()
public:
    DockType() = default;
    virtual ~DockType() = default;
    MARLIN_NONCOPYABLE(DockType);

    /// Its tab's label and its item in the Window menu.
    [[nodiscard]] virtual StringView GetDisplayName() const = 0;

    /// Whether it belongs in *context*, so its DockSpace may hold it and the Window menu lists it there.
    [[nodiscard]] virtual bool CanDockIn(const EditingContext &context) const = 0;

    /// The content, built under the dock's own owner when the dock opens and freed when it closes; moving or floating
    /// the dock keeps it. Empty by default.
    [[nodiscard]] virtual UIElement Build(EditingContext &context) const;
};

/// How the children of a split are laid out.
MENUM()
enum class DockAxis : uint8
{
    /// Left to right.
    Row,
    /// Top to bottom.
    Column,
};

/// One node of a dock layout, saved flat: a split of the nodes naming it as their parent, or a stack of docks shown
/// as tabs, one at a time.
MSTRUCT()
struct DockNodeState
{
    MGENERATED_BODY()

    /// The index of the split it is in; -1 for the root.
    MPROPERTY()
    int32 Parent = -1;
    /// Its share of the parent split, relative to its siblings.
    MPROPERTY()
    float32 Size = 1;
    MPROPERTY()
    bool Split = false;
    MPROPERTY()
    DockAxis Axis = DockAxis::Row;
    /// A stack's docks by type name, in tab order.
    MPROPERTY()
    List<String> Docks;
    /// The tab a stack shows.
    MPROPERTY()
    int32 Active = 0;
};

/// The docks of one area: the main area under an editing context's tab, or a floating window.
MSTRUCT()
struct DockAreaState
{
    MGENERATED_BODY()

    /// The main area's top row: the toolbar's stack, first, and the docks put to its right. Empty without a toolbar.
    MPROPERTY()
    List<DockNodeState> Top;
    /// Everything else; empty when no dock is open there.
    MPROPERTY()
    List<DockNodeState> Body;
    /// A floating window's client area, in physical pixels on the desktop.
    MPROPERTY()
    int32 X = 0;
    MPROPERTY()
    int32 Y = 0;
    MPROPERTY()
    int32 Width = 0;
    MPROPERTY()
    int32 Height = 0;
};

/// Where the docks of a DockSpace are: what its context lays out by default and what is saved per context type in
/// `/User/Editor/Layouts/<name>.mtext`.
MSTRUCT()
struct DockLayoutState
{
    MGENERATED_BODY()

    MPROPERTY()
    DockAreaState Main;
    MPROPERTY()
    List<DockAreaState> Floating;
};

/// Building flat dock layouts: each call adds a node to *nodes* and returns its index, for the nodes inside it.
namespace DockLayout
{
/// The name of the toolbar's dock in layouts.
inline constexpr StringView Toolbar = "Toolbar";

EDITORSHELL_API int32 AddSplit(List<DockNodeState> &nodes, int32 parent, DockAxis axis, float32 size = 1);
EDITORSHELL_API int32 AddStack(List<DockNodeState> &nodes, int32 parent, std::initializer_list<StringView> docks,
                               float32 size = 1);
} // namespace DockLayout
} // namespace Marlin
