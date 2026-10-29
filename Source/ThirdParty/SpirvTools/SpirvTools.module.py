from marlin.rules import *


class SpirvTools(ThirdPartyRules):
    """The C API of SPIRV-Tools, used to validate generated SPIR-V in tests; prebuilt for Windows X64."""

    version = "vulkan-sdk-1.4.363.0"
    url = "https://github.com/KhronosGroup/SPIRV-Tools/tree/vulkan-sdk-1.4.363.0"
    license = "Apache-2.0"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("only the Windows X64 binaries are checked in")
        self.add_shared_library("Bin/Windows/X64/SPIRV-Tools-shared.dll", "Lib/Windows/X64/SPIRV-Tools-shared.lib")
