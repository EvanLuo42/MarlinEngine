from marlin.rules import *


class EditorShell(ModuleRules):
    """The editor's main window (Documentation/Architecture/09 §5): its own title bar with a tab per editing context,
    the menu bar and the status bar; the editing contexts, their types and the menus; docks and their layouts."""

    public_dependencies = ["Core", "EditorCore", "EditorUI", "Engine"]
    private_dependencies = ["Application", "MarlinText", "UIText", "Vfs"]
