from marlin.rules import *


class FbxSceneDecoder(ModuleRules):
    """The SceneDecoder for FBX over ufbx, registered by reflection (Documentation/Architecture/13 §7.1)."""

    public_dependencies = ["AssetPipeline", "Core"]
    private_dependencies = ["Ufbx"]
