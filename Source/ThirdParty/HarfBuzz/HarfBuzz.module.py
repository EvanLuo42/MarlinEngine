from marlin.rules import *


class HarfBuzz(ThirdPartyRules):
    """HarfBuzz text shaping: the built-in OpenType shaper, hb-ot font funcs, glyph outlines (hb-draw) and COLR (hb-paint).

    Lib/ is built from the release sources by Build/BuildWindows.sh; the headers are the release's. No platform backends,
    no subsetter; allocation goes through MarlinHarfBuzzMalloc/Calloc/Realloc/Free, which the engine defines.
    """

    version = "15.0.0"
    url = "https://github.com/harfbuzz/harfbuzz/releases/tag/15.0.0"
    license = "MIT-Modern-Variant"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("HarfBuzz is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/HarfBuzz.lib")
