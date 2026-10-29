from marlin.rules import *


class UIIcon(ModuleRules):
    """Vector icons (Documentation/Architecture/09 §2): the Icon asset, baked from SVG by IconPipeline, and DrawIcon,
    which draws one with UIPainter. Like UIPainter it has no GPU code, so the Server target can link it.
    """

    public_dependencies = ["Asset", "Core", "UIPainter"]
