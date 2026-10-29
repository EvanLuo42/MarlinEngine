from marlin.rules import *


class CrashReport(ModuleRules):
    """Crash reporting inside the process: the crash handler, the state shared with MarlinCrashReporter, the report
    format and its privacy filters (Documentation/Architecture/03 §7.1)."""

    public_dependencies = ["Core"]
    private_dependencies = ["MarlinText"]
