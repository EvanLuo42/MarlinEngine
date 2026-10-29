"""Record what ``marlin generate`` reads, so ``marlin build`` can tell when to regenerate.

Inputs are recorded as a mapping of path to modification time, and for files also the hash of their
content, as ``[mtime, hash]``. An added or removed path, or a file whose content changed, means the
generated files no longer match the tree; a file only touched, as a checkout or a save without edits
does, is no change. The inputs are the rules files, the settings files, the toolkit sources that
generate runs (not the other commands'), the platform layers, the overlays of each module, and which
of ``Public/``, ``Internal/``, ``Private/`` and ``Tests/`` the module and its overlays have, since those
decide its include paths and test module; adding a header to an existing folder is not a change.
Adding a source file is, in a module with overlays: the generated build lists the sources that a
higher layer replaces.
"""

import hashlib
import os
from pathlib import Path

from marlin.build.discovery import TESTS_DIR, scan_rules
from marlin.build.graph import C_EXTENSIONS, CPP_EXTENSIONS, OBJC_EXTENSIONS, OWN_INCLUDE_DIRS, RC_EXTENSIONS
from marlin.paths import EnginePaths

_SOURCE_EXTENSIONS = CPP_EXTENSIONS + C_EXTENSIONS + OBJC_EXTENSIONS + RC_EXTENSIONS

_TOOLKIT = Path(__file__).resolve().parents[1]


def _is_generator_source(path: Path) -> bool:
    """Whether *path*, a toolkit source, is run by generate: not another command's, such as ``test``."""
    if "__pycache__" in path.parts or path.name == "__main__.py":
        return False
    commands = _TOOLKIT / "commands"
    return not path.is_relative_to(commands) or path.name in ("__init__.py", "generate.py")


def _input_files(paths: EnginePaths, rules: list[Path]) -> list[Path]:
    files = [*rules, *paths.build_settings_files, paths.version_file]
    if paths.presets.is_dir():
        files += paths.presets.glob("*.toml")
    files += (p for p in _TOOLKIT.rglob("*.py") if _is_generator_source(p))
    return files


def _content_hash(file: Path) -> str:
    return hashlib.blake2b(file.read_bytes(), digest_size=16).hexdigest()


def snapshot(paths: EnginePaths) -> dict[str, int | list[int | str]]:
    """Return the current inputs of generate."""
    result: dict[str, int | list[int | str]] = {}
    # One walk of the source trees finds both the rules files and the module directories.
    rules, modules = scan_rules(paths)
    for file in _input_files(paths, rules):
        try:
            result[str(file)] = [file.stat().st_mtime_ns, _content_hash(file)]
        except FileNotFoundError:
            continue
    result |= snapshot_layout(paths, modules)
    return result


def snapshot_layout(paths: EnginePaths, modules: list[list[Path]]) -> dict[str, int]:
    """Return the inputs that count by being there: platform layers, module roots and their folders, overlays."""
    result: dict[str, int] = {}
    if paths.platforms.is_dir():
        for layer in paths.platforms.iterdir():
            if layer.is_dir():
                result[f"{layer}{os.sep}"] = 0
    for roots in modules:
        for root in roots:
            result[f"{root}{os.sep}"] = 0
            for sub in (*OWN_INCLUDE_DIRS, TESTS_DIR):
                if (root / sub).is_dir():
                    result[f"{root / sub}{os.sep}"] = 0
            if len(roots) > 1:
                result |= dict.fromkeys(map(str, _sources(root)), 0)
    return result


def _sources(root: Path) -> list[Path]:
    """Return every source file below *root*, nested modules included; extra entries only cost a regenerate."""
    return [
        Path(directory, name)
        for directory, _, names in os.walk(root)
        for name in names
        if name.endswith(_SOURCE_EXTENSIONS)
    ]


def changes(paths: EnginePaths, recorded: dict[str, int | list[int | str]]) -> list[str]:
    """Return a description of each input that changed since *recorded*."""
    current = _stamps(paths)
    result = []
    for path in sorted(current.keys() | recorded.keys()):
        if path not in recorded:
            result.append(f"added {_relative(paths, path)}")
        elif path not in current:
            result.append(f"removed {_relative(paths, path)}")
        elif not _same(path, current[path], recorded[path]):
            result.append(f"modified {_relative(paths, path)}")
    return result


def _stamps(paths: EnginePaths) -> dict[str, int]:
    """The inputs as snapshot finds them, by modification time only; content is hashed only when a time differs."""
    result: dict[str, int] = {}
    rules, modules = scan_rules(paths)
    for file in _input_files(paths, rules):
        try:
            result[str(file)] = file.stat().st_mtime_ns
        except FileNotFoundError:
            continue
    for key, value in snapshot_layout(paths, modules).items():
        result[key] = value
    return result


def _same(path: str, mtime: int, recorded: int | list[int | str]) -> bool:
    """Whether the input *path*, now with *mtime*, is as *recorded*: the same time, or else the same content."""
    if isinstance(recorded, int):
        return mtime == recorded
    if mtime == recorded[0]:
        return True
    try:
        return _content_hash(Path(path)) == recorded[1]
    except OSError:
        return False


def _relative(paths: EnginePaths, path: str) -> str:
    file = Path(path)
    return str(file.relative_to(paths.root)) if file.is_relative_to(paths.root) else str(file)
