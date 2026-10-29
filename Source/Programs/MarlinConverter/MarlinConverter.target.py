from marlin.rules import *


class MarlinConverter(TargetRules):
    """A conversion worker (Documentation/Architecture/06 §3); the asset indexer starts them and hands them jobs."""

    type = TargetType.Program
    launch_module = "MarlinConverter"
    # Compression codecs, texture encoders and scene decoders, found through reflection (06 §3.4, §5.1; 13 §7.1).
    extra_modules = ["Lz4Codec", "ZstdCodec"]
    extra_modules += ["AstcTextureEncoder", "Bc7EncRdoEncoder", "IspcTextureEncoder"]
    extra_modules += ["FbxSceneDecoder", "GltfSceneDecoder"]
    # The components levels and prefabs may hold, found through reflection (05 §5).
    extra_modules += ["RenderComponents"]
    # InputAction and InputContext, read straight from their sources (Documentation/Architecture/15 §3).
    extra_modules += ["Input"]
