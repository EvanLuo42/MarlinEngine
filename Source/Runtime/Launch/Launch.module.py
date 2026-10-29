from marlin.rules import *


class Launch(ModuleRules):
    """The Game executable's entry point; Server and Editor get launch modules of their own (Documentation/Architecture/01)."""

    private_dependencies = ["Application", "Asset", "Core", "Engine", "RHI"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform in (Platform.Windows, Platform.Linux):
            self.private_dependencies += ["RHIVulkan"]
        if ctx.platform is Platform.Windows and ctx.arch is Arch.X64:
            self.private_dependencies += ["GpuCrashAftermath"]
        # Remote and loading from the DDC are development tools: Shipping builds leave them out by not linking them
        # (Documentation/Architecture/10, 06 §4.2).
        if not ctx.is_shipping:
            self.private_dependencies += ["GlobalStoreAssetStore", "Remote"]
