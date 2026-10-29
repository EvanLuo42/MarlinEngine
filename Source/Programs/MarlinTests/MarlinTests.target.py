from marlin.rules import *


class MarlinTests(TargetRules):
    """Runs the tests of every module (Documentation/Architecture/11); `marlin test` builds and runs it."""

    type = TargetType.Program
    launch_module = "MarlinTests"
    test_modules = True

    # RECOVERY (asset workspace): builds the asset modules that nothing linked here depends on yet.
    extra_modules = [
        "GlobalStoreAssetStore",
        "GlobalStoreScriptStore",
        "GlobalStoreShaderStore",
        "Mesh",
        "Shader",
        "Texture",
    ]
    # RECOVERY (uitext workspace): builds and tests UIText, which nothing linked here depends on yet.
    extra_modules += ["UIText"]
    # Builds and tests UIIcon, which nothing linked here depends on yet.
    extra_modules += ["UIIcon"]
    # Builds and tests the scene renderer and the game viewport, which nothing linked here depends on.
    extra_modules += ["SceneRenderer", "GameViewport"]
    # Builds and tests Input, and builds the OS devices, which nothing linked here depends on.
    extra_modules += ["Input", "ApplicationInput"]
    # RECOVERY (debugui workspace): builds and tests DebugUI, which nothing linked here depends on yet.
    extra_modules += ["DebugUI"]
    # RECOVERY (pipelines workspace): the pipeline and packaging modules and their tests.
    extra_modules += [
        "FontPipeline",
        "IconPipeline",
        "MaterialPipeline",
        "MeshPipeline",
        "Packaging",
        "ScriptPipeline",
        "ShaderPipeline",
        "TexturePipeline",
        "WorldPipeline",
    ]
    # RECOVERY (script workspace): the script backend and VM and their tests, which nothing linked here depends on yet.
    extra_modules += ["ScriptBackend", "ScriptVM"]

    def setup(self, ctx: BuildContext) -> None:
        # Built and reflected here though no test depends on them yet (recovery workspace).
        self.extra_modules += ["Renderer", "RenderGraphRemote"]
        # The RHI tests run on the backends found through reflection; `-Rhi=` picks one.
        if ctx.platform in (Platform.Windows, Platform.Linux):
            self.extra_modules += ["RHIVulkan"]
