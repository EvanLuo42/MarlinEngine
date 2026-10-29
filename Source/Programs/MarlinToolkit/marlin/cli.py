"""Entry point of the ``marlin`` command."""

import argparse
import sys

from marlin import __version__
from marlin.commands import build, clean, coverage, doc, generate, package, symbols, test
from marlin.paths import EnginePaths, find_engine_root

COMMANDS = (generate, build, clean, test, coverage, package, symbols, doc)


def make_parser() -> argparse.ArgumentParser:
    """Return the argument parser with every subcommand registered."""
    parser = argparse.ArgumentParser(prog="marlin", description="Marlin Engine toolkit.")
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    subparsers = parser.add_subparsers(dest="command", metavar="<command>", required=True)
    for command in COMMANDS:
        command.register(subparsers)
    return parser


def main(argv: list[str] | None = None) -> int:
    """Run the command line *argv* and return the exit code.

    Arguments after ``--`` are passed to the underlying tool unchanged. The parsed namespace keeps
    *argv*, so ``marlin generate`` can record its own arguments for ``marlin build`` to replay.
    """
    argv = sys.argv[1:] if argv is None else argv
    passthrough: list[str] = []
    if "--" in argv:
        split = argv.index("--")
        argv, passthrough = argv[:split], argv[split + 1 :]
    args = make_parser().parse_args(argv)
    args.passthrough = passthrough
    args.argv = argv
    try:
        paths = EnginePaths(find_engine_root())
    except RuntimeError as error:
        print(f"marlin: {error}", file=sys.stderr)
        return 1
    return args.handler(args, paths)


if __name__ == "__main__":
    sys.exit(main())
