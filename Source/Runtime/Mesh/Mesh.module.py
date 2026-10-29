from marlin.rules import *


class Mesh(ModuleRules):
    """Run-time static meshes as cluster DAGs streamed in pages (Documentation/Architecture/06, 07 §3)."""

    public_dependencies = ["Asset", "Core", "Material"]
