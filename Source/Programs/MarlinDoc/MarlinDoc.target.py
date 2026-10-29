from marlin.rules import *


class MarlinDoc(TargetRules):
    """Generates the manual and the API reference; `marlin doc` builds and runs it (Documentation/Architecture/16)."""

    type = TargetType.Program
    launch_module = "MarlinDoc"
