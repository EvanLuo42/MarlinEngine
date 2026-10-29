from marlin.rules import *


class GpuCaptureNsight(ModuleRules):
    """Nsight Graphics frame captures through the NGFX SDK (Documentation/Architecture/07 §1.7).

    Found through reflection as an IGpuCaptureTool; `-GpuCapture=Nsight` injects it at startup. The Nsight install is
    found at run time; Config can name one.
    """

    public_dependencies = ["Core", "Engine", "RHI"]
    private_dependencies = ["NsightGraphicsSdk", "Vfs"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform is not Platform.Windows:
            self.unsupported("NGFX injects Vulkan captures on Windows only")
