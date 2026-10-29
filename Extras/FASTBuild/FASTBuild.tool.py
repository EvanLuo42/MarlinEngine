from marlin.rules import *


class FASTBuild(ToolRules):
    """Runs the builds that `marlin generate` writes as .bff files."""

    version = "1.20"
    url = "https://github.com/fastbuild/fastbuild/releases/tag/v1.20"
    license = "Zlib"
