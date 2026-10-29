from marlin.rules import *


class RHIVulkan(ModuleRules):
    """The Vulkan 1.4 backend of the RHI (Documentation/Architecture/07 §1), with VK_EXT_descriptor_heap bindless.

    The loader is opened at run time, so a machine without Vulkan still starts with another backend. Its window
    surface and loader name come from the overlays of Platforms/<Layer>/Source/Runtime/RHIVulkan/.
    """

    public_dependencies = ["Core", "Engine", "RHI", "VulkanHeaders"]
    # Vulkan structures are written `VkFooInfo info{VK_STRUCTURE_TYPE_FOO_INFO};`, zeroing the rest on purpose.
    compile_options = ["-Wno-missing-field-initializers"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform not in (Platform.Windows, Platform.Linux):
            self.unsupported("Vulkan is the backend of Windows and Linux only")
