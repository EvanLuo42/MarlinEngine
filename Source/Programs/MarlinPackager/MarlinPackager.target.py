from marlin.rules import *


class MarlinPackager(TargetRules):
    """Packages a project for this platform (Documentation/Architecture/06 §5): converted objects, global shaders and
    config files into chunked, compressed and optionally encrypted .mpak files, plus the executable."""

    type = TargetType.Program
    launch_module = "MarlinPackager"
    # The compression codecs the platform's compression matrix names, found by name through reflection (06 §5.1).
    extra_modules = ["Lz4Codec", "ZstdCodec"]
