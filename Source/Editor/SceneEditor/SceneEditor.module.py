from marlin.rules import *


class SceneEditor(ModuleRules):
    """The scene editing context (Documentation/Architecture/09 §5): the tab Levels and Prefabs open in, its toolbar and
    its docks."""

    public_dependencies = ["Core", "EditorCore", "EditorShell", "EditorUI", "Engine"]
