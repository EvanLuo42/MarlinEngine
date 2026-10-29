from marlin.rules import *


class SceneRenderer(ModuleRules):
    """The scene renderer (Documentation/Architecture/07 §5): a RenderScene per World holding its GPU Scene, the
    SceneViews that observe it, the geometry pool of resident meshes, the textures materials sample, and the
    visibility-buffer passes that render each view into its frame image slot. Servers do not link it."""

    public_dependencies = [
        "Asset",
        "Core",
        "Engine",
        "Material",
        "Mesh",
        "RenderComponents",
        "RHI",
        "RenderGraph",
        "Renderer",
        "Shader",
        "Texture",
        "World",
    ]
