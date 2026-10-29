from marlin.rules import *


class MarlinCrashServer(ModuleRules):
    """Entry point of MarlinCrashServer."""

    private_dependencies = ["Config", "Core", "CrashServer", "Engine", "Net"]
