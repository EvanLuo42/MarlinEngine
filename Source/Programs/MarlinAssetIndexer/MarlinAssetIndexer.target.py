from marlin.rules import *


class MarlinAssetIndexer(TargetRules):
    """The per-project asset index daemon (Documentation/Architecture/06 §2); clients start it on demand."""

    type = TargetType.Program
    launch_module = "MarlinAssetIndexer"
