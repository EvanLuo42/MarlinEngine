from marlin.rules import *


class CrashServer(ModuleRules):
    """The crash report server: receives uploaded report folders, analyzes their minidumps against a symbol store and
    buckets them by signature (Documentation/Architecture/03 §7.1)."""

    public_dependencies = ["Core", "CrashReport", "Net"]
    private_dependencies = ["CrashAnalysis", "MarlinText"]
