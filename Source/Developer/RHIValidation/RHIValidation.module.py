from marlin.rules import *


class RHIValidation(ModuleRules):
    """Marlin's own RHI validation: a device layer that checks the RHI contract (Documentation/Architecture/07 §1.6).

    Enabled with -RHIValidation. As a developer module it is never in Shipping; targets link it through
    ``extra_modules`` outside Shipping.
    """

    public_dependencies = ["Core", "Engine", "RHI"]
