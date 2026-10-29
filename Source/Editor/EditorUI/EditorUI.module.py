from marlin.rules import *


class EditorUI(ModuleRules):
    """The editor's reactive UI (Documentation/Architecture/09 §4.1): signals with an owner tree, the element tree with
    flex and grid layout, and EditorUISubsystem, which runs it on the main window through UIPainter."""

    # RECOVERY-DECISION D1: not recovered; dependencies derived from the includes.
    public_dependencies = ["Application", "Asset", "Core", "EditorCore", "UIIcon", "UIPainter", "UIText"]
    private_dependencies = ["Config", "MarlinText", "UIPainterRenderer"]
