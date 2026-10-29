from marlin.rules import *


class Bc7EncRdo(ThirdPartyRules):
    """bc7enc_rdo: BC1-5 through rgbcx, BC7 through bc7e.ispc and the ert rate-distortion pass, behind the C interface in
    Include/MarlinBc7EncRdo.h.

    The repository has no releases. Lib/ is built from the commit in *url* by Build/BuildWindows.sh, which needs the ISPC
    compiler, together with Build/MarlinBc7EncRdo.cpp.
    """

    version = "b9438627eef73a1157e84201b6fa6eb2ffd6d9f0"
    url = "https://github.com/richgel999/bc7enc_rdo/tree/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0"
    license = "(MIT OR Unlicense) AND Apache-2.0 AND Zlib"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("bc7enc_rdo is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/Bc7EncRdo.lib")
