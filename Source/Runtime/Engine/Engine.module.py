from marlin.rules import *


class Engine(ModuleRules):
    """Subsystems, the run-time environment, startup and the main loop (Documentation/Architecture/01, 02)."""

    public_dependencies = ["Core", "Config", "MarlinText", "Vfs"]
    private_dependencies = ["CrashReport"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is Platform.Windows:
            # timeBeginPeriod, for the frame timer where there is no high-resolution waitable timer.
            self.system_libraries += ["winmm"]
            # Keeps <windows.h> and its macros, such as GetObject, out of the other sources.
            self.unity_exclude += ["Public/Engine/PlatformFrameTimer.cpp"]
