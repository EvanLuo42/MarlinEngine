from marlin.rules import *


class FontPipeline(ModuleRules):
    """FontSource, the font importer and the font converter, which keeps the sfnt file whole for HarfBuzz (09 §2,
    13 §7)."""

    public_dependencies = ["AssetPipeline", "Core", "MarlinText", "UIText"]
    private_dependencies = ["Vfs"]
