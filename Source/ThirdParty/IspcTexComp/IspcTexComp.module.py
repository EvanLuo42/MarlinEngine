from marlin.rules import *


class IspcTexComp(ThirdPartyRules):
    """Intel's ISPC Texture Compressor: BC1-7, BC6H, ETC1 and ASTC kernels for SSE2 and AVX.

    The repository has no releases. Lib/ is built from the commit in *url* by Build/BuildWindows.sh, which needs the ISPC
    compiler, as a static library; the header is the repository's.
    """

    version = "79ddbc90334fc31edd438e68ccb0fe99b4e15aab"
    url = "https://github.com/GameTechDev/ISPCTextureCompressor/tree/79ddbc90334fc31edd438e68ccb0fe99b4e15aab"
    license = "MIT"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("the ISPC Texture Compressor is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/IspcTexComp.lib")
