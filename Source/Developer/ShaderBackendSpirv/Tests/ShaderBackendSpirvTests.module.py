from marlin.rules import *


class ShaderBackendSpirvTests(ModuleRules):
    """Compiles shaders with the frontend and validates the SPIR-V with SPIRV-Tools."""

    private_dependencies = ["ShaderBackendSpirv", "LanguageFrontend", "SpirvTools", "Testing"]
