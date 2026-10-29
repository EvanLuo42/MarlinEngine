from marlin.rules import *


class SpirvTools(ToolRules):
    """spirv-val, spirv-dis, spirv-as, spirv-opt, spirv-cfg and spirv-link, for inspecting what the shader compiler
    writes; tests validate in process through the SpirvTools module instead."""

    version = "vulkan-sdk-1.4.363.0"
    url = "https://github.com/KhronosGroup/SPIRV-Tools/tree/vulkan-sdk-1.4.363.0"
    license = "Apache-2.0"
