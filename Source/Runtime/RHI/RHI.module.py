from marlin.rules import *


class RHI(ModuleRules):
    """The render hardware interface: devices, resources, pipelines, command lists and queues (Documentation/Architecture/07 §1).

    Backends (RHIVulkan, RHID3D12, ...), the validation layer and the GPU capture and crash tools implement its
    interfaces in modules of their own and are found through reflection.
    """

    public_dependencies = ["Core", "Engine"]
    private_dependencies = ["CrashReport", "Vfs"]
