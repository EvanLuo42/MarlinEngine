from marlin.rules import *


class ApplicationInput(ModuleRules):
    """The InputDevices of the OS (Documentation/Architecture/15 §2): keyboard, mouse and gamepad, from Application's
    events and gamepad state, found through reflection. Apart from Input so that what links Input without windows,
    such as the converter, does not link Application."""

    public_dependencies = ["Application", "Core", "Engine", "Input"]
