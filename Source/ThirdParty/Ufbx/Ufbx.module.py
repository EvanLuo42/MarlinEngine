from marlin.rules import *


class Ufbx(ThirdPartyRules):
    """ufbx: a single-file FBX reader, binary and ASCII.

    Lib/ is built from the release's ufbx.c by Build/BuildWindows.sh; the header is the release's.
    """

    version = "0.23.1"
    url = "https://github.com/ufbx/ufbx/releases/tag/v0.23.1"
    license = "MIT OR Unlicense"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("ufbx is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/Ufbx.lib")
