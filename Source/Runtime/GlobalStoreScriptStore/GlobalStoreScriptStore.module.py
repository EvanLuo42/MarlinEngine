from marlin.rules import *


class GlobalStoreScriptStore(ModuleRules):
    """Loads script modules in development through the asset indexer's Script.Locate, from GlobalStore
    (MarlinScript.md §11.1).

    Shipping builds do not link it, as with GlobalStoreAssetStore.
    """

    public_dependencies = ["Asset", "Core", "Engine", "GlobalStoreAssetStore", "Json", "Remote", "ScriptVM"]
