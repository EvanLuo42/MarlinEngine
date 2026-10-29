from marlin.rules import *


class MarlinPackager(ModuleRules):
    """Entry point of the packager: converts a project's content for this platform and writes its packages."""

    private_dependencies = ["AssetPipeline", "Config", "Core", "Engine", "MarlinText", "Packaging", "Vfs"]
