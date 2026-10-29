from marlin.rules import *


class GltfSceneDecoder(ModuleRules):
    """The SceneDecoder for glTF 2.0 over cgltf, registered by reflection (Documentation/Architecture/13 §7.1)."""

    public_dependencies = ["AssetPipeline", "Core"]
    private_dependencies = ["Cgltf", "MeshOptimizer"]
