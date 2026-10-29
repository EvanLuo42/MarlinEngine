from marlin.rules import *


class UIPainterRendererTests(ModuleRules):
    """The tests of UIPainterRenderer: its pass shaders compile to valid SPIR-V with the language toolchain."""

    private_dependencies = ["LanguageFrontend", "LanguageIR", "ShaderBackendSpirv", "SpirvTools"]
