"""Implement ``marlin coverage``: run tests under coverage and produce a report."""

import argparse

from marlin.commands import not_implemented
from marlin.paths import EnginePaths


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("coverage", help="Run tests with code coverage.")
    parser.add_argument("filters", nargs="*", help="only run tests matching these filters")
    parser.add_argument("--output", help="report directory (default: Saved/Coverage)")
    parser.set_defaults(handler=run)


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    return not_implemented("coverage")
