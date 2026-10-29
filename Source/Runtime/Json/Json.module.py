from marlin.rules import *


class Json(ModuleRules):
    """JSON values, parsing and writing, and reflected types as JSON and JSON Schema (Documentation/Architecture/10)."""

    public_dependencies = ["Core"]
