from marlin.rules import *


class SceneRendererTests(ModuleRules):
    """The tests of SceneRenderer: its pass shaders compile to valid SPIR-V with the language toolchain and match their
    C++ arguments, and the pieces of the GPU Scene that need no device."""

    private_dependencies = ["LanguageFrontend", "LanguageIR", "ShaderBackendSpirv", "SpirvTools"]
