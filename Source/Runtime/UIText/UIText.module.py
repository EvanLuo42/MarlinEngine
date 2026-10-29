from marlin.rules import *


class UIText(ModuleRules):
    """UI text (Documentation/Architecture/09 §2): the Font asset, and UITextSubsystem, which shapes UTF-8 with
    HarfBuzz into the UIShapedRuns that UIPainter draws and hands the GPU backend glyph outlines.
    """

    # RECOVERY-DECISION D1: dependencies derived from the includes; HarfBuzz stays private behind the module boundary.
    public_dependencies = ["Asset", "Core", "Engine", "UIPainter"]
    private_dependencies = ["HarfBuzz"]
