from marlin.rules import *


class Packaging(ModuleRules):
    """Packs converted objects, files, global shaders and script modules into packages
    (Documentation/Architecture/06 §5)."""

    public_dependencies = ["AssetPipeline", "Core", "GlobalStoreAssetStore", "ScriptPipeline", "ShaderPipeline", "Vfs"]
    private_dependencies = ["Asset", "Config", "ScriptVM", "Shader"]
