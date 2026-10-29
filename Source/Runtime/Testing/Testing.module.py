from marlin.rules import *


class Testing(ModuleRules):
    """The test framework every <Module>Tests module links (Documentation/Architecture/11)."""

    public_dependencies = ["Core"]
