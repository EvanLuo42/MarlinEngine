from marlin.rules import *


class AstcTextureEncoder(ModuleRules):
    """The TextureEncoder of ASTC 4x4, 6x6 and 8x8 through astcenc, registered by reflection (06 §3.4)."""

    public_dependencies = ["Core", "Texture", "TexturePipeline"]
    private_dependencies = ["Astcenc"]
