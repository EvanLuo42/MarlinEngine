from marlin.rules import *


class ClusterLod(ModuleRules):
    """Builds cluster LOD DAGs and their per-depth hierarchies in parallel, over meshoptimizer's clustering,
    partitioning and simplification (Documentation/Architecture/06 §3.5)."""

    public_dependencies = ["Core"]
    private_dependencies = ["MeshOptimizer"]
