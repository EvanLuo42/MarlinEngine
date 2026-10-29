from marlin.rules import *


class DebugUI(ModuleRules):
    """Immediate-mode debug widgets on UIPainter and the in-game debug overlay (Documentation/Architecture/09 §3): F1
    toggles the overlay, F2 the console window. Development tools only: Shipping targets do not link it.
    """

    # RECOVERY-DECISION D4: the docstring after "(Documentation/Architecture/09" and the dependency lists were lost;
    # dependencies are derived from the includes.
    public_dependencies = ["Core", "Engine"]
    private_dependencies = [
        "Application",
        "RHI",
        "RenderGraph",
        "Renderer",
        "UIPainter",
        "UIPainterRenderer",
        "UIText",
    ]
