from marlin.rules import *


class MarlinConverter(ModuleRules):
    """Entry point of the conversion worker; converter modules it links register their converters."""

    # RECOVERY-DECISION: the recovered list predates FontPipeline (commit 123), which is added here.
    private_dependencies = [
        "AssetPipeline",
        "Core",
        "Engine",
        "FontPipeline",
        "IconPipeline",
        "MaterialPipeline",
        "MeshPipeline",
        "ScriptPipeline",
        "ShaderPipeline",
        "TexturePipeline",
        "WorldPipeline",
    ]
