from marlin.rules import *


class AssetPipeline(ModuleRules):
    """The asset index, its scanner and service, conversion into GlobalStore, and the client tools use
    (Documentation/Architecture/06 §2, §3)."""

    public_dependencies = [
        "Asset",
        "Config",
        "Core",
        "Engine",
        "GlobalStore",
        "GlobalStoreAssetStore",
        "Json",
        "MarlinText",
        "Remote",
        "Vfs",
    ]
    # ConversionStoreDesc::InProcessDirectory runs a server in the process, for tests and tools.
    private_dependencies = ["GlobalStoreServer"]
