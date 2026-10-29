from marlin.rules import *


class Game(TargetRules):
    """The one game executable every project of this engine fork runs on (Documentation/Architecture/01)."""

    type = TargetType.Game
    launch_module = "Launch"

    def setup(self, ctx: BuildContext) -> None:
        # Debug and Development open a terminal with the log; Profile and Shipping are a window only.
        self.console = ctx.configuration in (Configuration.Debug, Configuration.Development)
        # UIPainter composes the main window; its subsystems (and the Renderer, RenderGraph and ShaderLibrary under them)
        # are found through reflection.
        self.extra_modules += ["UIPainterRenderer"]
        # The scene renderer and the game's view of its world, drawn under the UI (Documentation/Architecture/07 §5),
        # found through reflection.
        self.extra_modules += ["GameViewport"]
        # The local player's input and the keyboard, mouse and gamepad it reads (Documentation/Architecture/15), found
        # through reflection.
        self.extra_modules += ["Input", "ApplicationInput"]
        # Worlds and the startup level: WorldManagerSubsystem is found through reflection (Documentation/Architecture/05).
        self.extra_modules += ["World"]
        # The crash reporter Engine starts from the executable's folder, in every configuration.
        self.companion_targets += ["MarlinCrashReporter"]
        # Each World runs its scripts in a ScriptDomain; ScriptWorldSubsystem and ScriptModuleLoaderSubsystem, which
        # loads the bytecode modules from packages (from GlobalStore through GlobalStoreScriptStore in development), are
        # found through reflection (Documentation/Languages/MarlinScript.md §9.5, §11.1). Shipping carries the VM, never
        # the compiler (08 §4).
        self.extra_modules += ["ScriptVM"]
        # The compression codecs packages are read with, found by name through reflection (06 §5.1).
        self.extra_modules += ["Lz4Codec", "ZstdCodec"]
        # Development tools found through reflection, DebugUI's overlay among them; developer modules never reach
        # Shipping. RenderGraphRemote needs Remote, which Shipping does not link either.
        if not ctx.is_shipping:
            self.extra_modules += [
                "GlobalStoreScriptStore",
                "GlobalStoreShaderStore",
                "DebugUI",
                "RHIValidation",
                "RenderGraphRemote",
            ]
            if ctx.platform is Platform.Windows:
                self.extra_modules += ["GpuCaptureNsight"]
            # GlobalStoreAssetStore, under the stores above, starts the GlobalStore and the asset index daemon (and
            # through it the converters) from the executable's folder.
            self.companion_targets += ["MarlinAssetIndexer", "MarlinConverter", "MarlinGlobalStore"]
