from marlin.rules import *


# RECOVERY-DECISION D1: not recovered; dependencies mirror Launch plus EditorCore.
class EditorLaunch(ModuleRules):
    """The Editor executable's entry point; it links the editor scope (Documentation/Architecture/01)."""

    private_dependencies = [
        "Application",
        "Asset",
        "Core",
        "EditorCore",
        "Engine",
        "GlobalStoreAssetStore",
        "RHI",
        "Remote",
        "Splash",
    ]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform in (Platform.Windows, Platform.Linux):
            self.private_dependencies += ["RHIVulkan"]
        if ctx.platform is Platform.Windows and ctx.arch is Arch.X64:
            self.private_dependencies += ["GpuCrashAftermath"]
