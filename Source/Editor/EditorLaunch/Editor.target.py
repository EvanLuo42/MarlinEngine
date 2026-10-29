from marlin.rules import *


# RECOVERY-DECISION D1: the class line, docstring, type and launch_module are not recovered; setup() is.
class Editor(TargetRules):
    """The editor executable (Documentation/Architecture/01 §5.1, 09)."""

    type = TargetType.Editor
    launch_module = "EditorLaunch"

    def setup(self, ctx: BuildContext) -> None:
        self.console = ctx.configuration in (Configuration.Debug, Configuration.Development)
        # Subsystems found through reflection: the main window shell with its editing contexts (the Asset Manager and
        # the scene ones in their modules), the editor UI on UIPainter (and the renderer under it), worlds, and the
        # development tools Game links outside Shipping.
        self.extra_modules += ["AssetManager", "EditorShell", "EditorUI", "SceneEditor", "UIPainterRenderer", "World"]
        # Scripts: the VM per World (MarlinScript.md §9.5) and the compiler that turns MIR into its bytecode (08 §4).
        self.extra_modules += ["ScriptBackend", "ScriptVM"]
        # Compression codecs, texture encoders and scene decoders, found through reflection (06 §3.4, §5.1; 13 §7.1).
        self.extra_modules += ["Lz4Codec", "ZstdCodec"]
        self.extra_modules += ["AstcTextureEncoder", "Bc7EncRdoEncoder", "IspcTextureEncoder"]
        self.extra_modules += ["FbxSceneDecoder", "GltfSceneDecoder"]
        self.extra_modules += ["GlobalStoreScriptStore", "GlobalStoreShaderStore", "RHIValidation", "RenderGraphRemote"]
        if ctx.platform is Platform.Windows:
            self.extra_modules += ["GpuCaptureNsight"]
        # Programs it starts from its own folder: the asset index daemon (06 §2) and the converters it runs, the
        # GlobalStore and the crash reporter.
        self.companion_targets += ["MarlinAssetIndexer", "MarlinConverter", "MarlinGlobalStore", "MarlinCrashReporter"]
