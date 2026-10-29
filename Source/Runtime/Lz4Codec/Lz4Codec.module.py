from marlin.rules import *


class Lz4Codec(ModuleRules):
    """The Lz4 CompressionCodec, registered by reflection (Documentation/Architecture/06 §5.1)."""

    public_dependencies = ["Core"]
    private_dependencies = ["Lz4"]
