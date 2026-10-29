from marlin.rules import *


class CrashAnalysis(ModuleRules):
    """Reads minidumps, walks every thread's stack with the images' unwind data, symbolizes the frames and hashes a
    crash signature (Documentation/Architecture/03 §7.1). Used by MarlinCrashReporter and MarlinCrashServer.

    The symbol engine lives in the overlays of Platforms/<Layer>/Source/Runtime/CrashAnalysis/.
    """

    public_dependencies = ["Core"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is Platform.Windows:
            self.system_libraries += ["dbghelp"]
            # Keeps <windows.h> and its macros, such as GetExceptionCode, out of the other sources.
            self.unity_exclude += ["Private/WindowsCrashSymbols.cpp"]
