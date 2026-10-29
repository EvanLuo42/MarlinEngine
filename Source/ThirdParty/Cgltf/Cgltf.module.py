from marlin.rules import *


class Cgltf(ThirdPartyRules):
    """cgltf: a single-header glTF 2.0 reader.

    Lib/ holds its implementation, built from the release's cgltf.h by Build/BuildWindows.sh, so no engine source defines
    CGLTF_IMPLEMENTATION; the header is the release's.
    """

    version = "1.15"
    url = "https://github.com/jkuhlmann/cgltf/releases/tag/v1.15"
    license = "MIT"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("cgltf is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/Cgltf.lib")
