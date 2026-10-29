from marlin.rules import *


class MeshOptimizer(ThirdPartyRules):
    """meshoptimizer and its clusterlod builder: welding, tangents, meshlets and the cluster LOD DAG (07 §3).

    Lib/ is built from the release sources by Build/BuildWindows.sh; the headers are the release's.
    """

    version = "1.3"
    url = "https://github.com/zeux/meshoptimizer/releases/tag/v1.3"
    license = "MIT"
    public_include_paths = ["Include"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("meshoptimizer is only built for Windows x64; Build/ has the recipe")
        self.add_static_library("Lib/Windows/X64/MeshOptimizer.lib")
