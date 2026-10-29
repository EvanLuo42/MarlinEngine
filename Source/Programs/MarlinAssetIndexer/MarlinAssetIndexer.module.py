from marlin.rules import *


class MarlinAssetIndexer(ModuleRules):
    """Entry point of the asset index daemon."""

    private_dependencies = ["AssetPipeline", "Core", "Engine", "Shader", "ShaderPipeline"]
