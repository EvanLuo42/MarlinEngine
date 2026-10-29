"""The ``marlin`` subcommands.

Each module provides ``register(subparsers)``, which adds its parser and sets ``handler`` on it.
"""

import sys


def not_implemented(name: str) -> int:
    """Report that subcommand *name* is not implemented and return a failure exit code."""
    print(f"marlin {name}: not implemented yet", file=sys.stderr)
    return 1
