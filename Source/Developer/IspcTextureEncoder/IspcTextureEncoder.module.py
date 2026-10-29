from marlin.rules import *


class IspcTextureEncoder(ModuleRules):
    """The TextureEncoder of BC6H through Intel's ISPC Texture Compressor, registered by reflection (06 §3.4)."""

    public_dependencies = ["Core", "Texture", "TexturePipeline"]
    private_dependencies = ["IspcTexComp"]
