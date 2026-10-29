from marlin.rules import *


class GlobalStoreShaderStore(ModuleRules):
    """Loads global shaders in development through the asset indexer's Shader.Locate, from GlobalStore
    (MarlinLanguage.md §1.6).

    Shipping builds do not link it, as with GlobalStoreAssetStore.
    """

    public_dependencies = ["Asset", "Core", "Engine", "GlobalStoreAssetStore", "Json", "Remote", "Shader"]
