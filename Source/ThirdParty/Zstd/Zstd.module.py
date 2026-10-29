from marlin.rules import *


class Zstd(ThirdPartyRules):
    """Zstandard compression (06 §5.1).

    Lib/ is built from the release sources by Build/BuildWindows.sh; the headers are the release's.
    """

    version = "1.5.7"
    url = "https://github.com/facebook/zstd/releases/tag/v1.5.7"
    license = "BSD-3-Clause"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("the zstd library is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/Zstd.lib")
