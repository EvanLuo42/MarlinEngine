from marlin.rules import *


class ShaderBackendSpirv(ModuleRules):
    """Writes MIR as SPIR-V 1.6 for Vulkan with bindless descriptor heaps (Documentation/Languages/MarlinLanguage.md
    §11.3)."""

    public_dependencies = ["Core", "LanguageIR"]
    private_dependencies = ["SpirvHeaders"]
