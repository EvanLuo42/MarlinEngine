from marlin.rules import *


class LanguageIR(ModuleRules):
    """MIR, the SSA form with structured control flow that the language frontend lowers to and every backend reads
    (Documentation/Languages/MarlinLanguage.md §11.2)."""

    public_dependencies = ["Core"]
