from marlin.rules import *


class Bc7EncRdoEncoder(ModuleRules):
    """The TextureEncoder of BC1, BC3, BC4, BC5 and BC7 through bc7enc_rdo, registered by reflection (06 §3.4)."""

    public_dependencies = ["Core", "Texture", "TexturePipeline"]
    private_dependencies = ["Bc7EncRdo"]
