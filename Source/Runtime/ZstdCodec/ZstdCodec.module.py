from marlin.rules import *


class ZstdCodec(ModuleRules):
    """The Zstd CompressionCodec, registered by reflection (Documentation/Architecture/06 §5.1)."""

    public_dependencies = ["Core"]
    private_dependencies = ["Zstd"]
