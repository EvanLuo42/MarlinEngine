from marlin.rules import *


class MarlinDoc(ModuleRules):
    """Entry point of MarlinDoc: extracts `///` docs, reads the manual's books and writes the data and the static
    site (Documentation/Architecture/16)."""

    private_dependencies = ["Core", "Engine", "Json", "LanguageFrontend", "LibClang"]
