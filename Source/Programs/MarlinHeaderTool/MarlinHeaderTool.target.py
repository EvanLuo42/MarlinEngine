from marlin.rules import *


class MarlinHeaderTool(TargetRules):
    """Scans reflection markup and writes the .gen.h/.gen.cpp files (Documentation/Architecture/04)."""

    type = TargetType.Program
    launch_module = "MarlinHeaderTool"
