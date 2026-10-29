from marlin.rules import *


class Astcenc(ThirdPartyRules):
    """Arm's ASTC encoder: the astcenc codec library, without the command line tool, for AVX2.

    Lib/ is built from the release sources by Build/BuildWindows.sh; the header is the release's.
    """

    version = "5.7.0"
    url = "https://github.com/ARM-software/astc-encoder/releases/tag/5.7.0"
    license = "Apache-2.0"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("astcenc is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/Astcenc.lib")
