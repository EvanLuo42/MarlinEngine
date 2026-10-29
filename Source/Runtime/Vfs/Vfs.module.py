from marlin.rules import *


class Vfs(ModuleRules):
    """Virtual paths such as /Engine/Config, mounted onto directories and packages (Documentation/Architecture/02)."""

    public_dependencies = ["Core"]
