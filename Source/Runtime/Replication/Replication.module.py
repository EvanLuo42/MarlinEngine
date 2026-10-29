from marlin.rules import *


class Replication(ModuleRules):
    """Server-authoritative ECS snapshot replication over a NetTransport (Documentation/Architecture/05 §7, 14 §5)."""

    public_dependencies = ["Core", "Engine", "Net", "World"]
