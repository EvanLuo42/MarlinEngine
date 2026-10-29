from marlin.rules import *


class GpuCrashAftermath(ModuleRules):
    """GPU crash dumps on NVIDIA GPUs through Nsight Aftermath; shipped with the game (Documentation/Architecture/07 §1.7).

    Found through reflection as an IGpuCrashTool. Only Windows x64 has the Aftermath binaries checked in.
    """

    public_dependencies = ["Core", "Engine", "RHI"]
    private_dependencies = ["CrashReport", "Json", "NsightAftermath", "RHIVulkan", "Vfs"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows or ctx.arch is not Arch.X64:
            self.unsupported("the Aftermath binaries are only checked in for Windows x64")
