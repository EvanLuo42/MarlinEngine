from marlin.rules import *


class MeshPipeline(ModuleRules):
    """MeshSource, its importer (combined meshes or a mesh per scene mesh and a Prefab of the nodes) and the mesh
    converter, which builds cluster DAGs with ClusterLod (06 §3.5)."""

    public_dependencies = ["AssetPipeline", "Core", "MarlinText", "Mesh"]
    private_dependencies = ["ClusterLod", "MeshOptimizer", "RenderComponents", "World"]
