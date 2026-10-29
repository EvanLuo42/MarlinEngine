from marlin.rules import *


class MarlinEditorTests(TargetRules):
    """Runs the tests of every module, editor modules included (Documentation/Architecture/11): MarlinTests is a
    Program and cannot hold them."""

    type = TargetType.Editor
    launch_module = "MarlinTests"
    test_modules = True

    # The script compiler and VM, which no editor module links yet (MarlinScript.md §9.5, 08 §4).
    extra_modules = ["ScriptBackend", "ScriptVM"]

    def setup(self, ctx: BuildContext) -> None:
        if ctx.platform in (Platform.Windows, Platform.Linux):
            self.extra_modules += ["RHIVulkan"]
