from marlin.rules import *


class MarlinGlobalStore(ModuleRules):
    """Entry point of the GlobalStore server."""

    private_dependencies = ["Config", "Core", "Engine", "GlobalStoreServer", "Net"]
