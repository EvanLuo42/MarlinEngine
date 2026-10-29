from marlin.rules import *


class UIPainter(ModuleRules):
    """The recording side of UIPainter (Documentation/Architecture/09 §2): the immediate-mode painter, paths, brushes
    and the command stream that the GPU backend (UIPainterRenderer) bins into tiles and rasterizes.

    It has no GPU code, so the Server target can link it without the renderer.
    """

    public_dependencies = ["Core"]
