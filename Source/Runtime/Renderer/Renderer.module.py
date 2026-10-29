from marlin.rules import *


class Renderer(ModuleRules):
    """The frame: window surfaces, the frame stages render frame features add passes on, the image slots SceneViews
    hand to UIPainter, and the render job that builds, executes and presents each frame's graph while the next frame
    simulates (Documentation/Architecture/01 §4, 07 §5)."""

    public_dependencies = ["Application", "Core", "Engine", "RHI", "RenderGraph"]
