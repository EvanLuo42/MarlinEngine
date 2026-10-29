from marlin.rules import *


class ScriptBackend(ModuleRules):
    """Compiles MIR into script bytecode modules for the ScriptVM, and disassembles them
    (Documentation/Languages/MarlinScript.md §6, §14)."""

    public_dependencies = ["Core", "LanguageIR", "ScriptVM"]
