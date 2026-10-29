from marlin.rules import *


class ScriptPipeline(ModuleRules):
    """Compiles the script packages of the mounted source set into bytecode modules and stores them in GlobalStore
    (Documentation/Languages/MarlinScript.md §11.1)."""

    public_dependencies = ["AssetPipeline", "Core", "GlobalStoreAssetStore", "ScriptVM"]
    private_dependencies = ["Asset", "LanguageFrontend", "LanguageIR", "ScriptBackend", "Vfs"]
