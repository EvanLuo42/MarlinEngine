"""Implement ``marlin test``: build the test targets and run them.

A test target is any target with ``test_modules`` set; ``marlin generate`` records them in the
manifest. Each one is built for the host platform and arch, then run with the filters as its
arguments, followed by any arguments after ``--``.
"""

import argparse
import subprocess
import sys
from typing import Any

from marlin.build.model import Configuration, parse_enum
from marlin.build.platforms import Host
from marlin.commands.build import regenerate_if_stale
from marlin.fastbuild import load_manifest, run_fbuild
from marlin.paths import EnginePaths


def register(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser(
        "test",
        help="Build and run engine tests.",
        description="Build every test target (a target with test_modules set) for this host and run it. "
        "Filters and the arguments after `--` are passed to the test executables.",
    )
    parser.add_argument("filters", nargs="*", help="only run tests matching these filters")
    parser.add_argument("-c", "--configuration", default="Development", help="build configuration to test")
    parser.add_argument("-t", "--target", action="append", help="only this test target (repeatable)")
    parser.add_argument("--no-build", action="store_true", help="run the executables without building first")
    parser.add_argument("--list", action="store_true", help="list the test targets and their test modules")
    parser.set_defaults(handler=run)


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    """Build and run the selected test targets; return the exit code."""
    manifest = load_manifest(paths)
    if not manifest:
        print("marlin test: no build files; run `marlin generate` first", file=sys.stderr)
        return 1
    if not args.no_build:
        if (code := regenerate_if_stale(paths, manifest)) != 0:
            return code
        manifest = load_manifest(paths)
    try:
        configuration = parse_enum(Configuration, args.configuration)
    except ValueError as error:
        print(f"marlin test: {error}", file=sys.stderr)
        return 1

    entries = _select(manifest.get("tests", []), Host.detect(), configuration, args.target)
    if args.list:
        for entry in entries:
            print(f"{entry['alias']}: {', '.join(entry['modules']) or 'no test modules'}")
        return 0
    if not entries:
        print(
            f"marlin test: no test target for {configuration} on this host; "
            "set test_modules on a target and run `marlin generate`",
            file=sys.stderr,
        )
        return 1

    if not args.no_build:
        extra = list(manifest.get("fbuild_args", []))
        if (code := run_fbuild(paths, [e["alias"] for e in entries], extra)) != 0:
            return code
    failed = []
    for entry in entries:
        command = [entry["executable"], *args.filters, *args.passthrough]
        print(f"marlin test: running {entry['alias']}")
        sys.stdout.flush()
        try:
            code = subprocess.run(command, check=False, cwd=paths.root).returncode
        except OSError as error:
            print(f"marlin test: cannot run {entry['executable']}: {error}", file=sys.stderr)
            code = 1
        if code != 0:
            failed.append(entry["alias"])
    if failed:
        print(f"marlin test: failed: {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


def _select(
    entries: list[dict[str, Any]], host: Host, configuration: Configuration, targets: list[str] | None
) -> list[dict[str, Any]]:
    """Return the test targets of *entries* built for *host* and *configuration*, filtered by name."""
    wanted = {t.lower() for t in targets} if targets else None
    return [
        e
        for e in entries
        if e["platform"] == str(host.platform)
        and e["arch"] == str(host.arch)
        and e["configuration"] == str(configuration)
        and (wanted is None or e["target"].lower() in wanted)
    ]
