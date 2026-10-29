from marlin.rules import *


class MaterialPipeline(ModuleRules):
    """Material and MaterialInstance sources and their converter: reflects the `[Material]` struct through the shader
    pipeline and writes its parameter block (Documentation/Architecture/07 §4.2)."""

    public_dependencies = ["AssetPipeline", "Core", "MarlinText", "Material"]
    private_dependencies = ["Shader", "ShaderPipeline", "Texture"]
