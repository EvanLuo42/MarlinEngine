"""Implement ``marlin package``: build and stage a distributable engine."""

import argparse

from marlin.commands import not_implemented
from marlin.paths import EnginePaths


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser("package", help="Build and stage a distributable engine.")
    parser.add_argument("--configuration", default="Shipping", help="build configuration to package")
    parser.add_argument("--with-editor", action="store_true", help="include the editor")
    parser.add_argument("--output", help="staging directory (default: Saved/Packages)")
    parser.set_defaults(handler=run)


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    return not_implemented("package")
