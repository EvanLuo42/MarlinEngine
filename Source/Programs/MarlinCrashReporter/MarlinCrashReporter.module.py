from marlin.rules import *


class MarlinCrashReporter(ModuleRules):
    """Entry point of MarlinCrashReporter; its crash dialog lives in the overlays of
    Platforms/<Layer>/Source/Programs/MarlinCrashReporter/."""

    private_dependencies = ["Core", "CrashAnalysis", "CrashReport", "Engine", "Net"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is Platform.Windows:
            self.system_libraries += ["gdi32", "shell32", "user32"]
