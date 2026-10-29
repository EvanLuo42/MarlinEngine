from marlin.rules import *


class Asset(ModuleRules):
    """Run-time assets: Asset, AssetRef, the block format and the asset table (Documentation/Architecture/04, 06)."""

    public_dependencies = ["Core", "Engine", "Vfs"]
