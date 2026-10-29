from marlin.rules import *


class NsightAftermath(ThirdPartyRules):
    """NVIDIA Nsight Aftermath: GPU crash dumps on NVIDIA GPUs, shipped with the game (Documentation/Architecture/07 §1.7).

    Taken from the SDK in Nsight Graphics 2026.3.1. Linux uses the SDK's .tgz, which is not checked in yet.
    """

    version = "2026.3.26197.2318"
    url = "https://developer.nvidia.com/nsight-aftermath"
    license = "LicenseRef-NVIDIA-Nsight-Aftermath"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("the Aftermath binaries are only checked in for Windows x64")
        self.add_shared_library(
            "Bin/Windows/X64/GFSDK_Aftermath_Lib.x64.dll", "Lib/Windows/X64/GFSDK_Aftermath_Lib.x64.lib"
        )
