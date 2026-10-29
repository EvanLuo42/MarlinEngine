from marlin.rules import *


class Shader(ModuleRules):
    """Global shaders: the GlobalShader asset, declarations and the ShaderLibrary that dispatches them
    (Documentation/Languages/MarlinLanguage.md §1.6)."""

    public_dependencies = ["Asset", "Core", "Engine", "RHI"]
