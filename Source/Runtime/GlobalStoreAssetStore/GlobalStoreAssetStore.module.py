from marlin.rules import *


class GlobalStoreAssetStore(ModuleRules):
    """Loads converted assets in development straight from GlobalStore, located through the asset indexer
    (GlobalStore streaming, Documentation/Architecture/06 §4.2).

    Shipping builds do not link it: the launch modules depend on it only outside Shipping, as with Remote.
    """

    public_dependencies = ["Asset", "Core", "Engine", "GlobalStore", "Json", "Net", "Remote", "Vfs"]
    private_dependencies = ["Config"]
