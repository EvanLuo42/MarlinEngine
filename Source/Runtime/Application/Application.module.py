from marlin.rules import *


class Application(ModuleRules):
    """OS windows, input, gamepads, cursor, clipboard and monitors; not linked by servers or programs.

    Its platform code lives in the overlays of Platforms/<Layer>/Source/Runtime/Application/.
    """

    public_dependencies = ["Core", "Engine"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is Platform.Windows:
            self.system_libraries += ["dwmapi", "gdi32", "imm32", "shcore", "shell32", "user32", "xinput"]
