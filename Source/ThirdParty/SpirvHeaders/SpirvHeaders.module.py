from marlin.rules import *


class SpirvHeaders(ThirdPartyRules):
    """The SPIR-V opcodes and enums, GLSL.std.450 and NonSemantic.Shader.DebugInfo.100, header-only."""

    version = "vulkan-sdk-1.4.363.0"
    url = "https://github.com/KhronosGroup/SPIRV-Headers/tree/vulkan-sdk-1.4.363.0"
    license = "MIT"
    public_include_paths = ["Include"]
