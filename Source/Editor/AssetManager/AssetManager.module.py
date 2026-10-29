from marlin.rules import *


class AssetManager(ModuleRules):
    """The Asset Manager (Documentation/Architecture/09 §5): the pinned tab that browses the project's and the engine's
    assets through the asset index, with its folder, browser and details docks."""

    public_dependencies = ["Core", "EditorCore", "EditorShell", "EditorUI", "Engine"]
    private_dependencies = ["Application", "AssetPipeline", "MarlinText", "Net", "UIText", "Vfs"]
