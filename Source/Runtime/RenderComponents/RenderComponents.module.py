from marlin.rules import *


class RenderComponents(ModuleRules):
    """The components that make entities render (Documentation/Architecture/07 §5.2): RenderMesh, RenderInstance, the
    lights, Camera and its rigs such as FlyCamera. Apart from SceneRenderer so the converter, which links no renderer,
    knows them when it converts levels."""

    public_dependencies = ["Asset", "Core", "Input", "Mesh", "World"]
