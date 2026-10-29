from marlin.rules import *


class ScriptVM(ModuleRules):
    """The script VM (Documentation/Languages/MarlinScript.md §6): the bytecode format and module loading, the
    interpreter, the GC and ScriptDomain."""

    public_dependencies = ["Asset", "Core", "Engine", "World"]
