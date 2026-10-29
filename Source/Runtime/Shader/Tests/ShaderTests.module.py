from marlin.rules import *


class ShaderTests(ModuleRules):
    """The tests of Shader: reading the debug output buffer, on its own and as a shader compiled with the language
    toolchain writes it on the GPU."""

    private_dependencies = ["LanguageFrontend", "LanguageIR", "ShaderBackendSpirv", "SpirvTools"]
