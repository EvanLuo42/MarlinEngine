from marlin.rules import *


class Input(ModuleRules):
    """Game input (Documentation/Architecture/15): InputAction and InputContext assets, the InputDevice extension
    point and its controls, InputSubsystem, which turns the controls into action values each frame, and the systems
    that hand them to each world's stages. It links no Application, so the converter knows the assets; the devices
    that read the OS live in ApplicationInput."""

    public_dependencies = ["Asset", "Core", "Engine", "World"]
    private_dependencies = ["Config"]
