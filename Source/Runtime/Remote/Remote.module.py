from marlin.rules import *


class Remote(ModuleRules):
    """JSON-RPC 2.0 over named pipes, TCP and WebSocket for tools and other processes (Documentation/Architecture/10).

    Shipping builds do not link it: the launch modules depend on it only outside Shipping.
    """

    public_dependencies = ["Core", "Engine", "Json", "Net"]
