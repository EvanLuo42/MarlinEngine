"""Implement ``marlin build``: run FASTBuild, regenerating the .bff first if it is stale."""

import argparse
import os
import sys
import time
from typing import Any

from marlin.bff import write_if_changed
from marlin.build import inputs
from marlin.fastbuild import load_manifest, run_fbuild
from marlin.paths import EnginePaths


def register(subparsers: argparse._SubParsersAction) -> None:
    """Add the ``build`` subcommand to *subparsers*."""
    parser = subparsers.add_parser(
        "build",
        help="Build targets with FASTBuild.",
        description="Build targets with FASTBuild. Targets are aliases such as "
        "MarlinEditor-Windows-X64-Development or All-Windows-X64-Debug. If a rules file, build settings "
        "file or preset changed since `marlin generate`, it is re-run first with the same arguments. "
        "Cache and distribution follow what `marlin generate` decided; arguments after `--` are passed "
        "to FBuild as-is.",
    )
    parser.add_argument("targets", nargs="*", help="FASTBuild targets (default: all)")
    parser.add_argument("--clean", action="store_true", help="force a full rebuild")
    parser.add_argument("--no-cache", action="store_true", help="skip the FASTBuild cache for this build")
    parser.add_argument("--no-dist", action="store_true", help="build locally only for this build")
    parser.add_argument(
        "--no-regenerate", action="store_true", help="build with the existing .bff even if rules changed"
    )
    parser.add_argument("--ide", action="store_true", help="IDE-friendly output (used by the VS projects)")
    parser.add_argument("--list", action="store_true", help="list the generated targets and exit")
    parser.add_argument("-j", "--jobs", type=int, help="number of local worker threads")
    parser.set_defaults(handler=run)


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    """Build the targets in *args*; return the exit code."""
    manifest = load_manifest(paths)
    if manifest and not args.no_regenerate:
        code = regenerate_if_stale(paths, manifest)
        if code != 0:
            return code
        manifest = load_manifest(paths)
    if args.list:
        for alias in manifest.get("aliases", []):
            print(alias)
        return 0
    if manifest:
        unity = manifest.get("settings", {}).get("unity", {})
        write_isolate_list(paths, unity.get("isolate_max", 0), unity.get("isolate_hours", 0))
    extra = []
    for flag in manifest.get("fbuild_args", []):
        if flag.startswith("-cache") and args.no_cache or flag == "-dist" and args.no_dist:
            continue
        extra.append(flag)
    if args.ide:
        extra.append("-ide")
    if args.clean:
        extra.append("-clean")
    if args.jobs is not None:
        extra.append(f"-j{args.jobs}")
    return run_fbuild(paths, args.targets, [*extra, *args.passthrough])


_ISOLATED_EXTENSIONS = (".cpp", ".cc", ".cxx", ".c")
_SOURCE_TREES = ("Source", "Platforms", "Samples")


def write_isolate_list(paths: EnginePaths, isolate_max: int, isolate_hours: int) -> None:
    """List the sources edited in the last *isolate_hours* for FASTBuild to compile outside their unity files.

    Editing one of them again then recompiles only it, not the other files of its unity file. More than
    *isolate_max* of them, as after a sync, is a bulk change that unity files build faster, so none are listed.
    The list stays the same while the same files are being worked on; a file leaves it once its edit is old.
    """
    if isolate_max <= 0:
        return
    since = time.time() - isolate_hours * 3600
    recent: list[tuple[float, str]] = []
    for tree in _SOURCE_TREES:
        for directory, folders, names in os.walk(paths.root / tree):
            folders[:] = [f for f in folders if not f.startswith(".") and f != "__pycache__"]
            for name in names:
                if not name.endswith(_ISOLATED_EXTENSIONS):
                    continue
                path = os.path.join(directory, name)
                try:
                    modified = os.stat(path).st_mtime
                except OSError:
                    continue
                if modified >= since:
                    recent.append((modified, path))
    listed = sorted(path for _, path in recent) if len(recent) <= isolate_max else []
    write_if_changed(paths.unity_isolate_list, "".join(f"{path}\n" for path in listed))


def regenerate_if_stale(paths: EnginePaths, manifest: dict[str, Any]) -> int:
    """Re-run the recorded ``marlin generate`` if an input changed; return its exit code."""
    changes = inputs.changes(paths, manifest.get("inputs", {}))
    if not changes:
        return 0
    invocation: list[str] = manifest.get("invocation", [])
    shown = ", ".join(changes[:3]) + (f" and {len(changes) - 3} more" if len(changes) > 3 else "")
    print(f"marlin build: {shown}; running `{' '.join(['marlin generate', *invocation])}`")
    sys.stdout.flush()
    from marlin.cli import make_parser

    generate_args = make_parser().parse_args(["generate", *invocation])
    generate_args.passthrough = []
    generate_args.argv = ["generate", *invocation]
    return generate_args.handler(generate_args, paths)
