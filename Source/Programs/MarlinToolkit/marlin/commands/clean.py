"""Implement ``marlin clean``: delete generated and built files.

Nothing outside the engine root is deleted, and no symbolic link is followed out of it.
"""

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

from marlin.bff import compdb
from marlin.build.discovery import rules_files
from marlin.build.settings import SettingsError, load_settings
from marlin.paths import EnginePaths

VS_SUFFIXES = (".vcxproj", ".vcxproj.filters")
GENERATED_SUFFIXES = (*VS_SUFFIXES, ".vcxproj.user", ".sln", ".gta.runsettings", compdb.FILE_NAME)


def register(subparsers: argparse._SubParsersAction) -> None:
    """Add the ``clean`` subcommand to *subparsers*."""
    parser = subparsers.add_parser(
        "clean",
        help="Delete build outputs and generated project files.",
        description="Delete Intermediate/, Binaries/, the generated .sln and .vcxproj files, compile_commands.json "
        "and the .vs/ folder. "
        "Saved/ (which holds the local FASTBuild cache by default) and per-user .vcxproj.user files "
        "are only removed with --all. Nothing outside the engine root is ever touched.",
    )
    parser.add_argument("--all", action="store_true", help="also delete Saved/ and .vcxproj.user files")
    parser.add_argument("-n", "--dry-run", action="store_true", help="list what would be deleted")
    parser.set_defaults(handler=run)


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    """Delete, or with ``--dry-run`` list, what *args* selects; return the exit code."""
    targets = _targets(paths, args.all)
    if not targets:
        print("Nothing to clean.")
        return 0
    failures = 0
    freed = 0
    for target in targets:
        size = _size(target)
        label = f"{target.relative_to(paths.root)}{os.sep if target.is_dir() else ''}"
        if args.dry_run:
            print(f"would delete {label} ({_human(size)})")
            freed += size
            continue
        try:
            if target.is_dir():
                shutil.rmtree(target)
            else:
                target.unlink()
        except OSError as error:
            print(f"marlin clean: could not delete {label}: {error.strerror or error}", file=sys.stderr)
            failures += 1
            continue
        print(f"deleted {label} ({_human(size)})")
        freed += size
    print(f"{'Would free' if args.dry_run else 'Freed'} {_human(freed)}.")
    return 1 if failures else 0


def _targets(paths: EnginePaths, everything: bool) -> list[Path]:
    """Return the existing paths to delete, all inside the engine root.

    Only the fixed output folders are deleted as trees; anything else must be a file with a generated suffix, so a
    damaged manifest cannot name a source folder.
    """
    folders = [paths.intermediate, paths.binaries, paths.root / ".vs"]
    if everything:
        folders.append(paths.saved)
    suffixes = (*VS_SUFFIXES, ".vcxproj.user") if everything else VS_SUFFIXES
    files = [paths.root / compdb.FILE_NAME, *_solution_files(paths), *_project_files(paths, suffixes)]
    root = paths.root.resolve()
    result: list[Path] = []
    for candidate in dict.fromkeys([*folders, *files]):
        if candidate.is_symlink() or candidate.is_junction() or not candidate.exists():
            continue
        if not candidate.resolve().is_relative_to(root) or candidate.resolve() == root:
            continue
        if candidate in folders or (candidate.is_file() and candidate.name.endswith(GENERATED_SUFFIXES)):
            result.append(candidate)
    return result


def _solution_files(paths: EnginePaths) -> list[Path]:
    """Return the solution the manifest records and the one the settings name, each with the test adapter
    settings generate writes beside it."""
    files = []
    manifest = _manifest(paths)
    if solution := manifest.get("solution"):
        files.append(paths.resolve(solution))
    try:
        settings = load_settings(paths.build_settings_files, [])
        files.append(paths.root / f"{settings.solution.name}.sln")
    except SettingsError:
        pass
    return [*files, *(f.with_suffix(".gta.runsettings") for f in files)]


def _project_files(paths: EnginePaths, suffixes: tuple[str, ...]) -> list[Path]:
    """Return the project files the manifest records and those next to any rules file."""
    stems = [paths.resolve(p).with_suffix("") for p in _manifest(paths).get("projects", [])]
    stems += [file.parent / file.name.split(".")[0] for file in rules_files(paths)]
    return [stem.with_name(stem.name + suffix) for stem in dict.fromkeys(stems) for suffix in suffixes]


def _manifest(paths: EnginePaths) -> dict:
    try:
        return json.loads(paths.generated_manifest.read_text(encoding="utf-8"))
    except OSError, ValueError:
        return {}


def _size(path: Path) -> int:
    """Return the size of *path* in bytes, including everything below a directory."""
    if path.is_file():
        return path.stat().st_size
    total = 0
    for file in path.rglob("*"):
        try:
            if file.is_file() and not file.is_symlink():
                total += file.stat().st_size
        except OSError:
            continue
    return total


def _human(size: float) -> str:
    """Return *size* in bytes formatted with a binary unit."""
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} GiB"
