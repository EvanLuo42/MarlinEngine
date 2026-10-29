from marlin.rules import *


class EditorCore(ModuleRules):
    """The editor process's scope: EditorSubsystem and the engine subsystem that hosts it
    (Documentation/Architecture/01 §3, 09)."""

    public_dependencies = ["Core", "Engine"]
