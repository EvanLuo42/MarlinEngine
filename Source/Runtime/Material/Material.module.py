from marlin.rules import *


class Material(ModuleRules):
    """Run-time materials: a `[Material]` struct, its variant and its parameter block as the material table holds it
    (Documentation/Architecture/07 §4)."""

    public_dependencies = ["Asset", "Core", "Texture"]
