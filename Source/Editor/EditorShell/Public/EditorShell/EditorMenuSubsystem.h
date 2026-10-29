#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "EditorUI/Signal.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"
#include "EditorShell/EditorMenuSubsystem.gen.h"

namespace Marlin
{
/// One item of a menu of the menu bar.
struct EditorMenuItem
{
    String Label;
    /// Shown right-aligned, such as "Ctrl+S"; the shortcut itself is bound elsewhere.
    String Shortcut;
    /// Lower sorts first; items of equal order keep the order they were added in.
    int32 Order = 0;
    Function<void()> Execute;
    /// Unbound means always enabled.
    Function<bool()> IsEnabled;
    /// Asked when the menu opens; unbound means always shown.
    Function<bool()> IsVisible;
};

/// The menus of the main window's menu bar (09 §4.3): which menus there are comes from config, so projects and
/// plugins add menus without code; what is in them is added by subsystems at startup (AddItem).
MCLASS(Config = "Editor.Menus")
class EDITORSHELL_API EditorMenuSubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    /// The menu bar's menus, left to right.
    MPROPERTY()
    static List<String> MenuBar;

    EditorMenuSubsystem();
    ~EditorMenuSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    /// Adds *item* to *menu*, which need not be on the menu bar (it then shows nowhere).
    void AddItem(StringView menu, EditorMenuItem item);

    /// The items of *menu* in order; follows changes.
    [[nodiscard]] Span<const EditorMenuItem> GetItems(StringView menu) const;

private:
    struct Menu
    {
        String Name;
        List<EditorMenuItem> Items;
    };

    ReactiveOwner *_owner = nullptr;
    Signal<uint32> _version;
    List<Menu> _menus;
};
} // namespace Marlin
