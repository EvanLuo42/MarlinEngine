from marlin.rules import *


class ScriptBackendTests(ModuleRules):
    """Compiles scripts with the frontend and checks the bytecode the script target writes."""

    private_dependencies = ["ScriptBackend", "LanguageFrontend", "Testing"]
