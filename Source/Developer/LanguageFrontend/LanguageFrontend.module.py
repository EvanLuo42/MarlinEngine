from marlin.rules import *


class LanguageFrontend(ModuleRules):
    """The shared front half of the Marlin shader and script compilers: lexing, parsing, semantic analysis and
    monomorphization, lowering to MIR (Documentation/Languages/MarlinLanguage.md §11.1)."""

    public_dependencies = ["Core", "LanguageIR"]
    private_dependencies = ["Vfs"]
