from marlin.rules import *


class MarlinHeaderTool(ModuleRules):
    """Standalone and std-based, so it can generate code for Core itself; the STL is used without exceptions."""

    private_definitions = ["_HAS_EXCEPTIONS=0"]
