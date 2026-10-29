from marlin.rules import *


class VulkanHeaders(ThirdPartyRules):
    """The Vulkan C headers (vulkan/, vk_video/), header-only; the C++ bindings and layer/ICD headers are left out.

    Taken from the Vulkan SDK's Include folder. Functions are loaded at run time, so prototypes are off.
    """

    version = "vulkan-sdk-1.4.363.0"
    url = "https://github.com/KhronosGroup/Vulkan-Headers/tree/vulkan-sdk-1.4.363.0"
    license = "Apache-2.0 OR MIT"
    public_include_paths = ["Include"]
    public_definitions = ["VK_NO_PROTOTYPES"]
