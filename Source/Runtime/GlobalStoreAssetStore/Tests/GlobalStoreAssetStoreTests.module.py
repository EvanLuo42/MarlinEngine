from marlin.rules import *


class GlobalStoreAssetStoreTests(ModuleRules):
    """Reads converted results from an in-process GlobalStore server, as GlobalStore streaming does."""

    private_dependencies = ["GlobalStoreAssetStore", "GlobalStoreServer", "Testing"]
