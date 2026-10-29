from marlin.rules import *


class Texture(ModuleRules):
    """Run-time 2D textures whose mips stream from bulk data, and their formats (Documentation/Architecture/06 §4.1)."""

    public_dependencies = ["Asset", "Core"]
