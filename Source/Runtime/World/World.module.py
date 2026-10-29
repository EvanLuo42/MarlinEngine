from marlin.rules import *


class World(ModuleRules):
    """Worlds and the archetype ECS: entities, queries, systems, hierarchy, templates and partition streaming
    (Documentation/Architecture/05)."""

    public_dependencies = ["Asset", "Core", "Engine"]
    private_dependencies = ["Config", "MarlinText"]
