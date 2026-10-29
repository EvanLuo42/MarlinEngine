from marlin.rules import *


class ShaderPipeline(ModuleRules):
    """Compiles global shaders for the profiles of Engine.Shaders and stores them in GlobalStore
    (Documentation/Languages/MarlinLanguage.md §1.6)."""

    public_dependencies = ["AssetPipeline", "Core", "GlobalStoreAssetStore", "Shader"]
    private_dependencies = ["Asset", "Config", "LanguageFrontend", "LanguageIR", "ShaderBackendSpirv", "Vfs"]
