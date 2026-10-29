from marlin.rules import *


class GameViewport(ModuleRules):
    """The game's view of its world (Documentation/Architecture/07 §5.5): a SceneView of the startup world drawn over
    the whole main window, beneath the game's UI, seen from the world's Camera, which FlyCameraSystem flies from its
    input actions. The game target links it; the editor has viewports of its own."""

    public_dependencies = [
        "Core",
        "Engine",
        "Input",
        "RenderComponents",
        "SceneRenderer",
        "UIPainter",
        "UIPainterRenderer",
        "World",
    ]
