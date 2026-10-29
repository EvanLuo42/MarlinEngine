from marlin.rules import *


class GlobalStore(ModuleRules):
    """The client of a GlobalStore key-value server, where converted data lives (Documentation/Architecture/06 §3)."""

    public_dependencies = ["Core", "Net"]
