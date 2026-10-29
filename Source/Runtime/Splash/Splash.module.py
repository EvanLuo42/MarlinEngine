from marlin.rules import *


class Splash(ModuleRules):
    """The splash screen shown while the engine starts, until the main window can draw a whole frame
    (Documentation/Architecture/09 §4.6). A target links it and hands Splash::GetStartupSplash() to LaunchMain.
    """

    public_dependencies = ["Core", "Engine"]
    private_dependencies = ["Application", "Asset"]
