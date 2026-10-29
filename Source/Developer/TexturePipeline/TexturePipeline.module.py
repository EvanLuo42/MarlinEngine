from marlin.rules import *


class TexturePipeline(ModuleRules):
    """TextureSource, the PNG decoder, the TextureEncoder extension point and the texture converter (06 §3.4)."""

    public_dependencies = ["AssetPipeline", "Core", "MarlinText", "Texture"]
    private_dependencies = ["Config"]
