from marlin.rules import *


class MarlinGlobalStore(TargetRules):
    """The GlobalStore server program, shared by a studio or run locally (Documentation/Architecture/06 §3)."""

    type = TargetType.Program
    launch_module = "MarlinGlobalStore"
