from marlin.rules import *


class ShaderPipelineTests(ModuleRules):
    """Compiles global shaders and validates the SPIR-V with SPIRV-Tools; converts them through GlobalStore."""

    private_dependencies = ["Config", "ShaderPipeline", "SpirvTools", "Testing", "Vfs"]
