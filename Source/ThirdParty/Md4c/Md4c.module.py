from marlin.rules import *


class Md4c(ThirdPartyRules):
    """md4c: a CommonMark parser with callbacks, used by MarlinDoc to render Markdown (Documentation/Architecture/16 §5).

    Lib/ is built from the release's md4c.c by Build/BuildWindows.sh; the header is the release's. It allocates with
    malloc, so only programs may link it (Documentation/Architecture/12 §2).
    """

    version = "0.5.2"
    url = "https://github.com/mity/md4c/releases/tag/release-0.5.2"
    license = "MIT"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("md4c is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/Md4c.lib")
