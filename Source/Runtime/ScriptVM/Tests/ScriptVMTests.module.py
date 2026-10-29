from marlin.rules import *


class ScriptVMTests(ModuleRules):
    """Runs scripts compiled by the frontend and ScriptBackend on the VM, and every script `[Test]` (MarlinScript
    §15)."""

    private_dependencies = ["ScriptVM", "ScriptBackend", "LanguageFrontend", "Testing"]
