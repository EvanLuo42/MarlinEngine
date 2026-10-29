from marlin.rules import *


class NsightGraphicsSdk(ThirdPartyRules):
    """The Nsight Graphics SDK (NGFX): header-only code that finds Nsight Graphics installs and injects its capture.

    The libraries it loads come from the user's Nsight Graphics install at run time; none are checked in.
    Taken from the SDK in Nsight Graphics 2026.3.1.
    """

    version = "0.9.2"
    url = "https://developer.nvidia.com/nsight-graphics"
    license = "Apache-2.0"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is Platform.Windows:
            # Its install search reads the registry and the known folders.
            self.system_libraries += ["advapi32", "shell32"]
