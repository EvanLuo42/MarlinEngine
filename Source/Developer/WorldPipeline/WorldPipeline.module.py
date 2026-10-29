from marlin.rules import *


class WorldPipeline(ModuleRules):
    """The Prefab and Level converters, which flatten prefabs into entity templates and levels into streaming cells
    (Documentation/Architecture/05 §5-6)."""

    public_dependencies = ["AssetPipeline", "Core", "MarlinText", "World"]
