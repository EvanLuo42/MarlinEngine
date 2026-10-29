from marlin.rules import *


class IconPipeline(ModuleRules):
    """IconSource, the SVG importer and the icon converter, which bakes an SVG subset into the shapes of an Icon
    (06 §3, 09 §2)."""

    public_dependencies = ["AssetPipeline", "Core", "MarlinText", "UIIcon"]
    private_dependencies = ["UIPainter", "Vfs"]
