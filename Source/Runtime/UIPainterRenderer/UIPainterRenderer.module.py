from marlin.rules import *


class UIPainterRenderer(ModuleRules):
    """The GPU backend of UIPainter (Documentation/Architecture/09 §2): window canvases, glyph cache and atlas, and the
    RenderGraph passes that bin each canvas's commands into 16×16 tiles and rasterize them with analytic coverage
    into the window's swapchain. Servers do not link it."""

    public_dependencies = [
        "Application",
        "Core",
        "Engine",
        "RHI",
        "RenderGraph",
        "Renderer",
        "Shader",
        "UIPainter",
        "UIText",
    ]
