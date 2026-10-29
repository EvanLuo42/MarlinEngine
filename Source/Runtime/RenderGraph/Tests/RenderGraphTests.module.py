from marlin.rules import *


class RenderGraphTests(ModuleRules):
    """The tests of RenderGraph. The compiler, transient pool and executor are private headers of the module, exported
    so the tests can include them from ../Private and drive hand-built frames on a real device. Raster tests compile
    their shaders with the language toolchain."""

    private_include_paths = ["../Private"]
    private_dependencies = ["LanguageFrontend", "LanguageIR", "ShaderBackendSpirv"]
