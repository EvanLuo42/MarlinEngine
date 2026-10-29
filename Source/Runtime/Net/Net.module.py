from marlin.rules import *


class Net(ModuleRules):
    """Streams, listeners and datagrams on IO threads over a completion port (Documentation/Architecture/14)."""

    public_dependencies = ["Core", "Engine"]
