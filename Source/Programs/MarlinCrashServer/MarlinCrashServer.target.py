from marlin.rules import *


class MarlinCrashServer(TargetRules):
    """The crash report server program, run by a studio or locally (Documentation/Architecture/03 §7.1)."""

    type = TargetType.Program
    launch_module = "MarlinCrashServer"
