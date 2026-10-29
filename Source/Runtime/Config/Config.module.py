from marlin.rules import *


class Config(ModuleRules):
    """Layered MarlinText config bound to reflected types (Documentation/Architecture/06 §1.4)."""

    public_dependencies = ["Core", "MarlinText", "Vfs"]
