from marlin.rules import *


class Core(ModuleRules):
    """Base types, memory, containers, strings, jobs, math and diagnostics; every module depends on it.

    Its platform code lives in the overlays of Platforms/<Layer>/Source/Runtime/Core/.
    """

    pch = "Private/CorePCH.h"

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is Platform.Windows:
            self.system_libraries += [
                "advapi32",
                "bcrypt",
                "dbghelp",
                "ole32",
                "shell32",
                "synchronization",
                "user32",
                "ws2_32",
            ]
