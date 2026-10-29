#pragma once

#include "CoreTypes.h"
#include "EditorUI/Signal.h"
#include "EditorUI/UIElement.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"
#include "EditorShell/EditingContext.gen.h"

namespace Marlin
{
class EditingContextType;

/// One tab of the main window (Documentation/Architecture/09 §5): the Asset Manager, the editing of one asset (an
/// AssetContext) or a tool that edits no particular asset (a ToolContext). Made by EditingContextSubsystem under an
/// owner of its own, so its signals and its content live until it closes. Its content is built the first time it is
/// shown and kept, hidden, while another tab is active.
class EDITORSHELL_API EditingContext
{
public:
    explicit EditingContext(const EditingContextType *type);
    virtual ~EditingContext() = default;
    MARLIN_NONCOPYABLE(EditingContext);

    /// The tab's label.
    Signal<String> Title;
    /// Theme tokens of the tab's icon and its color.
    String Icon;
    String IconColor;

    /// The type that made it; null for the Asset Manager.
    [[nodiscard]] const EditingContextType *GetType() const
    {
        return _type;
    }

    /// Pinned contexts stay first in the tab bar, cannot be dragged and have no close button.
    [[nodiscard]] virtual bool IsPinned() const
    {
        return false;
    }

    /// The asset it edits, or an invalid Guid.
    [[nodiscard]] virtual Guid GetAsset() const
    {
        return {};
    }

    /// The UI under the tab; built once, under the context's owner, the first time the tab is shown.
    [[nodiscard]] virtual UIElement BuildContent() = 0;

    /// Became, or stopped being, the active tab.
    virtual void OnActivated()
    {
    }
    virtual void OnDeactivated()
    {
    }

    /// Asked before closing; false keeps it open (unsaved changes).
    [[nodiscard]] virtual bool CanClose()
    {
        return true;
    }

private:
    const EditingContextType *_type = nullptr;
};

/// A kind of editing context: what its tabs look like and how to make one (Provider, 13 §3). Stateless.
MCLASS(Abstract, ExtensionPoint)
class EDITORSHELL_API EditingContextType
{
    MGENERATED_BODY()
public:
    EditingContextType() = default;
    virtual ~EditingContextType() = default;
    MARLIN_NONCOPYABLE(EditingContextType);

    /// Theme tokens of the tab's icon and its color.
    [[nodiscard]] virtual StringView GetIcon() const = 0;
    [[nodiscard]] virtual StringView GetIconColor() const = 0;
};

/// The editing of one asset, opened from the Asset Manager: a tab per asset, each with its own docks, selection and
/// undo (09 §5). The type with the highest Priority whose CanEdit accepts the asset's type makes it.
MCLASS(Abstract, ExtensionPoint)
class EDITORSHELL_API AssetContextType : public EditingContextType
{
    MGENERATED_BODY()
public:
    /// Whether it edits assets whose source type is *assetType*, such as "TextureSource"; empty when unknown.
    [[nodiscard]] virtual bool CanEdit(StringView assetType) const = 0;
    /// A context for *asset*, titled *name*; called with the new context's owner current.
    [[nodiscard]] virtual UniquePtr<EditingContext> Create(const Guid &asset, StringView name) const = 0;
};

/// A context that is always open, first in the tab bar and cannot be dragged or closed, such as the Asset Manager:
/// EditingContextSubsystem opens one of each linked type at startup, highest Priority first.
MCLASS(Abstract, ExtensionPoint)
class EDITORSHELL_API PinnedContextType : public EditingContextType
{
    MGENERATED_BODY()
public:
    /// The item of the Window menu that shows it.
    [[nodiscard]] virtual StringView GetDisplayName() const = 0;
    /// The context, which must say IsPinned; called with the new context's owner current.
    [[nodiscard]] virtual UniquePtr<EditingContext> Create() const = 0;
};

/// A tool that edits no particular asset, such as a debugger, opened from a menu: at most one tab per type.
MCLASS(Abstract, ExtensionPoint)
class EDITORSHELL_API ToolContextType : public EditingContextType
{
    MGENERATED_BODY()
public:
    /// The tab's title and the menu item that opens it.
    [[nodiscard]] virtual StringView GetDisplayName() const = 0;
    /// The menu of the menu bar the item is in (Editor.Menus.MenuBar).
    [[nodiscard]] virtual StringView GetMenu() const
    {
        return "Window";
    }
    /// The context; called with the new context's owner current.
    [[nodiscard]] virtual UniquePtr<EditingContext> Create() const = 0;
};
} // namespace Marlin
