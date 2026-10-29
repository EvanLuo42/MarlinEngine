from marlin.rules import *


class Lz4(ThirdPartyRules):
    """LZ4 block compression and its high-compression mode: the engine's default package codec (06 §5).

    Lib/ is built from the release sources by Build/BuildWindows.sh; the headers are the release's.
    """

    version = "1.10.0"
    url = "https://github.com/lz4/lz4/releases/tag/v1.10.0"
    license = "BSD-2-Clause"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("the lz4 library is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/Lz4.lib")
