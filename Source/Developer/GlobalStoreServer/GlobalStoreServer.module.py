from marlin.rules import *


class GlobalStoreServer(ModuleRules):
    """The GlobalStore server: memory first, segment files behind, optionally in front of an upstream server."""

    public_dependencies = ["Core", "GlobalStore"]
