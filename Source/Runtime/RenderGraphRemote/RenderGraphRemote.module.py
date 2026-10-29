from marlin.rules import *


class RenderGraphRemote(ModuleRules):
    """The Render.Graph and Render.CapturePasses Remote methods over RenderGraph (Documentation/Architecture/07 §2, 10).

    It needs Remote, which Shipping builds do not link, so targets add it outside Shipping.
    """

    public_dependencies = ["Core", "Engine", "Remote", "RenderGraph"]
    private_dependencies = ["Json", "RHI"]
