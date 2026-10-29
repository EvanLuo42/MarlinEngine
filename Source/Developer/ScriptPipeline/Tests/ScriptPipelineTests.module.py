from marlin.rules import *


class ScriptPipelineTests(ModuleRules):
    """Compiles the engine's script package, converts it through GlobalStore and loads it as an asset."""

    private_dependencies = ["Asset", "ScriptPipeline", "Testing", "Vfs"]
