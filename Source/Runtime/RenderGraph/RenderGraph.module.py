from marlin.rules import *


class RenderGraph(ModuleRules):
    """Per-frame pass graphs over the RHI: barriers, transient aliasing, async queues, culling and job-parallel
    recording (Documentation/Architecture/07 §2).

    The Remote export of compiled graphs is in RenderGraphRemote, which Shipping builds do not link.
    """

    public_dependencies = ["Core", "Engine", "RHI"]
