from marlin.rules import *


class MarlinCrashReporter(TargetRules):
    """Watches a process from outside, writes its minidump when it crashes or hangs and uploads the report
    (Documentation/Architecture/03 §7.1)."""

    type = TargetType.Program
    launch_module = "MarlinCrashReporter"
