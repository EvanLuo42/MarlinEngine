"""Load the build settings from Build/BuildSettings.toml, presets, local overrides and ``--set``.

Settings are resolved at generate time and written into the .bff. Unknown keys are errors, so a
misspelled key never falls back to its default silently.
"""

import dataclasses
import tomllib
import types
import typing
from dataclasses import dataclass, field
from enum import StrEnum
from pathlib import Path
from typing import Any

from marlin.build.model import Configuration, LinkMode, Optimize


@dataclass
class ConfigurationSettings:
    """Settings of one configuration.

    *link* applies to every target, so the editor, the game and the programs share compiled modules.
    """

    optimize: Optimize = Optimize.speed
    lto: bool = False
    debug_info: bool = True
    assertions: bool = True
    sanitizers: list[str] = field(default_factory=list)
    definitions: list[str] = field(default_factory=list)
    link: LinkMode = LinkMode.Monolithic


def _default_configurations() -> dict[Configuration, ConfigurationSettings]:
    return {
        Configuration.Debug: ConfigurationSettings(optimize=Optimize.none, link=LinkMode.Modular),
        Configuration.Development: ConfigurationSettings(link=LinkMode.Modular),
        Configuration.Profile: ConfigurationSettings(assertions=False, lto=True),
        Configuration.Shipping: ConfigurationSettings(assertions=False, lto=True),
    }


@dataclass
class UnitySettings:
    """Unity build settings; modules with fewer than *min_files* sources compile file by file.

    Sources edited in the last *isolate_hours* compile on their own, outside their unity file, so editing one again
    recompiles only it; when more than *isolate_max* were edited, as after a sync, none are (0 turns this off).
    """

    enabled: bool = True
    files_per_unit: int = 16
    min_files: int = 4
    isolate_max: int = 32
    isolate_hours: int = 8


@dataclass
class PchSettings:
    enabled: bool = True


class CacheMode(StrEnum):
    """Whether builds read from the FASTBuild cache, write to it, or both."""

    read = "read"
    write = "write"
    readwrite = "readwrite"


@dataclass
class CacheSettings:
    """FASTBuild cache settings.

    *path* is relative to the engine root, or absolute, including UNC paths. If *mount_point* is
    set, FASTBuild uses the cache only while that volume is mounted, so a disconnected network
    drive does not turn into a local folder.
    """

    enabled: bool = True
    path: str = "Saved/FASTBuildCache"
    mode: CacheMode = CacheMode.readwrite
    mount_point: str = ""


@dataclass
class DistributionSettings:
    """FASTBuild distributed compilation settings."""

    enabled: bool = False
    brokerage_path: str = ""
    workers: list[str] = field(default_factory=list)


@dataclass
class ToolchainSettings:
    """Toolchain locations.

    *llvm_root* is relative to the engine root or absolute. If empty, LLVM is looked up in
    ``$LLVM_ROOT``, the default install location and ``PATH``.
    """

    llvm_root: str = ""


class VisualStudioVersion(StrEnum):
    """The Visual Studio release a solution targets; ``auto`` picks the newest complete install."""

    auto = "auto"
    vs2019 = "2019"
    vs2022 = "2022"
    vs2026 = "2026"


@dataclass
class SolutionSettings:
    """Visual Studio solution settings."""

    enabled: bool = True
    name: str = "Marlin"
    visual_studio: VisualStudioVersion = VisualStudioVersion.auto
    # The .mproject, relative to the engine root, that Editor, Game and Server targets (but not test runners) open
    # when started from the IDE; empty starts them without -Project.
    debug_project: str = ""


@dataclass
class CompileCommandsSettings:
    """compile_commands.json settings; each source is listed as *configuration* compiles it."""

    enabled: bool = True
    configuration: Configuration = Configuration.Development


@dataclass
class BuildSettings:
    """All settings that ``marlin generate`` reads."""

    cpp_standard: str = "c++20"
    c_standard: str = "c17"
    warnings_as_errors: bool = False
    unity: UnitySettings = field(default_factory=UnitySettings)
    pch: PchSettings = field(default_factory=PchSettings)
    cache: CacheSettings = field(default_factory=CacheSettings)
    distribution: DistributionSettings = field(default_factory=DistributionSettings)
    toolchain: ToolchainSettings = field(default_factory=ToolchainSettings)
    solution: SolutionSettings = field(default_factory=SolutionSettings)
    compile_commands: CompileCommandsSettings = field(default_factory=CompileCommandsSettings)
    configurations: dict[Configuration, ConfigurationSettings] = field(default_factory=_default_configurations)

    def to_dict(self) -> dict[str, Any]:
        """Return the settings as plain TOML-compatible values."""
        return _dump(self)


class SettingsError(Exception):
    """Raised for a settings file or override that cannot be read."""


def find_presets(directory: Path, names: list[str]) -> list[Path]:
    """Return the preset files in *directory* for *names*, matched case-insensitively."""
    available = {p.stem.lower(): p for p in sorted(directory.glob("*.toml"))} if directory.is_dir() else {}
    files = []
    for name in names:
        if (path := available.get(name.lower())) is None:
            choices = ", ".join(p.stem for p in available.values()) or "none"
            raise SettingsError(f"unknown preset '{name}' (available: {choices})")
        files.append(path)
    return files


def load_settings(files: list[Path], overrides: list[str]) -> BuildSettings:
    """Return the defaults overlaid by *files* and then by ``key.path=value`` *overrides*.

    Later files win and missing files are skipped. A table only overrides the keys it names.
    """
    merged: dict[str, Any] = _dump(BuildSettings())
    for path in files:
        if path.is_file():
            try:
                with path.open("rb") as stream:
                    _deep_merge(merged, tomllib.load(stream))
            except tomllib.TOMLDecodeError as error:
                raise SettingsError(f"{path}: {error}") from error
    for override in overrides:
        _deep_merge(merged, _parse_override(override))
    return _convert(BuildSettings, merged, "")


def _parse_override(text: str) -> dict[str, Any]:
    """Return ``key.path=value`` as nested tables; a value that is not TOML is taken as a string."""
    key, sep, value = text.partition("=")
    if not sep or not key.strip():
        raise SettingsError(f"--set expects key=value, got '{text}'")
    try:
        parsed = tomllib.loads(f"v = {value}")["v"]
    except tomllib.TOMLDecodeError:
        parsed = value
    result: dict[str, Any] = {}
    node = result
    parts = key.strip().split(".")
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = parsed
    return result


def _deep_merge(base: dict[str, Any], overlay: dict[str, Any]) -> None:
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value


def _convert(annotation: Any, value: Any, where: str) -> Any:
    """Return *value* converted to *annotation*, or raise :exc:`SettingsError` naming *where*.

    Enum settings also accept a bare TOML integer, so ``visual_studio = 2026`` needs no quotes.
    """
    origin = typing.get_origin(annotation)
    if dataclasses.is_dataclass(annotation):
        if not isinstance(value, dict):
            raise SettingsError(f"{where or '<root>'}: expected a table")
        hints = typing.get_type_hints(annotation)
        names = {f.name for f in dataclasses.fields(annotation)}
        unknown = set(value) - names
        if unknown:
            raise SettingsError(f"{where or '<root>'}: unknown key(s) {', '.join(sorted(unknown))}")
        kwargs = {name: _convert(hints[name], value[name], _join(where, name)) for name in names if name in value}
        return annotation(**kwargs)
    if origin is list:
        (item,) = typing.get_args(annotation)
        if not isinstance(value, list):
            raise SettingsError(f"{where}: expected an array")
        return [_convert(item, v, f"{where}[{i}]") for i, v in enumerate(value)]
    if origin is dict:
        key_type, value_type = typing.get_args(annotation)
        if not isinstance(value, dict):
            raise SettingsError(f"{where}: expected a table")
        return {
            _convert(key_type, k, _join(where, k)): _convert(value_type, v, _join(where, k)) for k, v in value.items()
        }
    if isinstance(annotation, type) and issubclass(annotation, StrEnum):
        text = str(value) if isinstance(value, int) and not isinstance(value, bool) else value
        for member in annotation:
            if isinstance(text, str) and member.value.lower() == text.lower():
                return member
        choices = ", ".join(m.value for m in annotation)
        raise SettingsError(f"{where}: '{value}' is not one of {choices}")
    if annotation is bool:
        if not isinstance(value, bool):
            raise SettingsError(f"{where}: expected true/false")
        return value
    if annotation is int:
        if isinstance(value, bool) or not isinstance(value, int):
            raise SettingsError(f"{where}: expected an integer")
        return value
    if annotation is str:
        if not isinstance(value, str):
            raise SettingsError(f"{where}: expected a string")
        return value
    if isinstance(annotation, types.UnionType):
        raise SettingsError(f"{where}: unsupported union type")
    raise SettingsError(f"{where}: unsupported setting type {annotation!r}")


def _join(where: str, key: str) -> str:
    return f"{where}.{key}" if where else str(key)


def _dump(value: Any) -> Any:
    if dataclasses.is_dataclass(value):
        return {f.name: _dump(getattr(value, f.name)) for f in dataclasses.fields(value)}
    if isinstance(value, dict):
        return {str(k): _dump(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_dump(v) for v in value]
    return str(value) if isinstance(value, StrEnum) else value
