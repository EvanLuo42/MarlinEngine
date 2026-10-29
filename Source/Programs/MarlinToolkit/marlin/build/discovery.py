"""Find and load the ``*.module.py``, ``*.target.py`` and ``*.tool.py`` rules files.

Rules files are searched in Source/ and in each Platforms/<Layer>/Source/, which keeps code under
restricted SDK licenses out of Source/. A module's kind follows the top-level folder it is in.

A layer (see :mod:`marlin.build.platforms`) also adds to the modules of the same tree defined below
it: ``Platforms/<Layer>/Source/Runtime/Core/`` is an overlay of ``Source/Runtime/Core/``, laid out
like a module but without a rules file. Which overlays a build uses, and in what order, follows the
layer stack of its platform and arch; Source/ itself ranks below every layer.

A ``Tests/`` folder directly inside a module holds that module's tests. It becomes the test module
``<Module>Tests``, with a ``<Module>Tests.module.py`` of its own only if it needs more than the
defaults (see :mod:`marlin.build.graph`). An overlay's ``Tests/`` folder is an overlay of the test
module.

Every folder of Extras/ is a checked-in tool described by its ``<Name>.tool.py``, with its programs in one
folder per platform and arch, ``Extras/<Name>/<Platform>/<Arch>/``. Tools and third-party modules must declare
their version, url and license.
"""

import dataclasses
import importlib.util
import inspect
import os
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass
from enum import StrEnum
from functools import cached_property
from pathlib import Path
from typing import Any

from marlin.build.model import Arch, Platform
from marlin.build.platforms import LAYERS
from marlin.paths import EnginePaths
from marlin.rules import ModuleRules, TargetRules, ThirdPartyRules, ToolRules

MODULE_SUFFIX = ".module.py"
TARGET_SUFFIX = ".target.py"
TOOL_SUFFIX = ".tool.py"
TESTS_DIR = "Tests"
SKIPPED_DIRS = {"__pycache__", ".venv", "Intermediate", "Binaries", "Saved"}


class ModuleKind(StrEnum):
    """Where a module lives, which decides who may depend on it."""

    Runtime = "Runtime"
    Developer = "Developer"
    Editor = "Editor"
    Program = "Program"
    ThirdParty = "ThirdParty"
    Test = "Test"


@dataclass(frozen=True)
class ModuleDescriptor:
    """A discovered module and its rules class.

    A test module has *tested_module* set; its rules file need not exist, in which case *rules_class*
    holds the defaults. *layer* is the layer the module is defined in, ``None`` for Source/; *overlays*
    maps each layer that adds to the module to its overlay directory.
    """

    name: str
    kind: ModuleKind
    rules_file: Path
    rules_class: type[ModuleRules | ThirdPartyRules]
    tested_module: str | None = None
    layer: str | None = None
    overlays: Mapping[str, Path] = dataclasses.field(default_factory=dict, compare=False)

    @cached_property
    def dir(self) -> Path:
        """The module directory, which holds the rules file."""
        return self.rules_file.parent

    @property
    def is_third_party(self) -> bool:
        return issubclass(self.rules_class, ThirdPartyRules)

    def roots(self, stack: Iterable[str]) -> list[Path]:
        """Return the directories that make up the module on the layer *stack*, highest first.

        Only layers above the module's own add to it; the module directory comes last.
        """
        stack = list(stack)
        above = stack[: stack.index(self.layer)] if self.layer in stack else stack
        return [*(self.overlays[layer] for layer in above if layer in self.overlays), self.dir]


@dataclass(frozen=True)
class TargetDescriptor:
    """A discovered target and its rules class."""

    name: str
    rules_file: Path
    rules_class: type[TargetRules]


@dataclass(frozen=True)
class ToolDescriptor:
    """A discovered tool under Extras/ and its rules class."""

    name: str
    rules_file: Path
    rules_class: type[ToolRules]

    @cached_property
    def dir(self) -> Path:
        """The tool directory, which holds the rules file."""
        return self.rules_file.parent


class DiscoveryError(Exception):
    """Raised for a rules file that cannot be loaded, or a name defined twice."""


@dataclass
class Discovered:
    """The modules, targets and Extras/ tools found in the tree, by name.

    *layers* are the engine layers present under ``Platforms/``; a platform needs all of its own.
    """

    modules: dict[str, ModuleDescriptor]
    targets: dict[str, TargetDescriptor]
    tools: dict[str, ToolDescriptor] = dataclasses.field(default_factory=dict)
    layers: set[str] = dataclasses.field(default_factory=set)

    @cached_property
    def test_modules(self) -> list[ModuleDescriptor]:
        return [m for m in self.modules.values() if m.tested_module is not None]

    @cached_property
    def module_dirs(self) -> set[Path]:
        """The module directories and their overlays, whose sources belong to that module alone."""
        return {d for m in self.modules.values() for d in (m.dir, *m.overlays.values())}

    @cached_property
    def nested_module_dirs(self) -> dict[Path, list[Path]]:
        """Map each of :attr:`module_dirs` to the others below it."""
        return nested_dirs(self.module_dirs)


def nested_dirs(dirs: Iterable[Path]) -> dict[Path, list[Path]]:
    """Map each of *dirs* to the other directories of *dirs* below it, sorted.

    Each directory's parents are looked up rather than every pair compared, which would take
    quadratic time on large trees.
    """
    result: dict[Path, list[Path]] = {d: [] for d in dirs}
    for directory in result:
        for parent in directory.parents:
            if parent in result:
                result[parent].append(directory)
    return {d: sorted(nested) for d, nested in result.items()}


def discover(paths: EnginePaths) -> Discovered:
    """Load every rules file of the engine.

    Raise :exc:`DiscoveryError` if a rules file is invalid.
    """
    discovered = Discovered({}, {}, layers={d.name for d in _layer_dirs(paths.platforms, validate=True)})
    for root in _source_roots(paths.source, paths.platforms):
        _load_source_root(discovered, root)
    _add_test_modules(discovered.modules)
    _check_overlays(discovered.modules)
    discovered.tools = _discover_tools(paths.extras)
    return discovered


def _discover_tools(extras: Path) -> dict[str, ToolDescriptor]:
    """Load the ``<Name>.tool.py`` that every folder of *extras* must have."""
    tools = {}
    if not extras.is_dir():
        return tools
    for directory in sorted(p for p in extras.iterdir() if p.is_dir() and not p.name.startswith(".")):
        file = directory / f"{directory.name}{TOOL_SUFFIX}"
        if not file.is_file():
            raise DiscoveryError(f"{directory}: a tool folder needs {file.name}")
        rules = _load_rules_class(file, (ToolRules,))
        _check_package_info(file, rules)
        _check_tool_layout(directory)
        tools[directory.name] = ToolDescriptor(directory.name, file, rules)
    return tools


def _check_tool_layout(directory: Path) -> None:
    """Reject folders of a tool that are not ``<Platform>/<Arch>/``."""
    platforms = {p.value for p in Platform}
    arches = {a.value for a in Arch}
    for child in sorted(p for p in directory.iterdir() if p.is_dir() and p.name not in SKIPPED_DIRS):
        if child.name not in platforms:
            known = ", ".join(sorted(platforms))
            raise DiscoveryError(f"{child}: a tool's folders are platforms ({known}), each holding arch folders")
        for arch in sorted(p for p in child.iterdir() if p.is_dir()):
            if arch.name not in arches:
                known = ", ".join(sorted(arches))
                raise DiscoveryError(f"{arch}: expected an arch folder ({known})")


def _check_package_info(file: Path, rules: type[ThirdPartyRules | ToolRules]) -> None:
    missing = [field for field in ("version", "url", "license") if not getattr(rules, field)]
    if missing:
        raise DiscoveryError(f"{file}: the package must declare {', '.join(missing)}")


def rules_files(paths: EnginePaths) -> list[Path]:
    """Return every rules file of the engine."""
    return scan_rules(paths)[0]


def module_roots(paths: EnginePaths) -> list[list[Path]]:
    """Return the directory and overlays of every module of the engine.

    Test modules without a rules file of their own are not included.
    """
    return scan_rules(paths)[1]


def scan_rules(paths: EnginePaths) -> tuple[list[Path], list[list[Path]]]:
    """Return :func:`rules_files` and :func:`module_roots` from one walk of the tree."""
    files: list[Path] = []
    roots: list[list[Path]] = []
    for tree in _source_trees(paths):
        layers = _layer_dirs(tree.platforms)
        for file in _walk(tree.path):
            files.append(file)
            if file.name.endswith(MODULE_SUFFIX):
                roots.append([file.parent, *_overlays(file.parent, tree, layers).values()])
    return files, roots


@dataclass(frozen=True)
class _SourceRoot:
    """A folder that holds rules files: a Source/ folder, or that of layer *layer* below *platforms*."""

    path: Path
    platforms: Path
    layer: str | None


def _source_trees(paths: EnginePaths) -> list[_SourceRoot]:
    return _source_roots(paths.source, paths.platforms)


def _layer_dirs(platforms: Path, validate: bool = False) -> list[Path]:
    """Return the layer folders below *platforms*; with *validate*, reject unknown layer names."""
    if not platforms.is_dir():
        return []
    dirs = sorted(p for p in platforms.iterdir() if p.is_dir() and not p.name.startswith("."))
    if validate:
        for directory in dirs:
            if directory.name not in LAYERS:
                known = ", ".join(sorted(LAYERS))
                raise DiscoveryError(
                    f"{directory}: '{directory.name}' is not a platform layer (expected one of: {known})"
                )
    return dirs


def _source_roots(source: Path, platforms: Path) -> list[_SourceRoot]:
    """Return *source* and the ``Source/`` folder of each layer below *platforms*."""
    roots = [_SourceRoot(source, platforms, None)]
    roots += [_SourceRoot(d / "Source", platforms, d.name) for d in _layer_dirs(platforms) if (d / "Source").is_dir()]
    return roots


def _overlays(module_dir: Path, root: _SourceRoot, layers: list[Path] | None = None) -> dict[str, Path]:
    """Return the overlay directory of *module_dir* in each other layer of its tree, by layer.

    *layers* are the layer folders of the tree, when the caller has them already.
    """
    relative = module_dir.relative_to(root.path)
    return {
        d.name: overlay
        for d in (_layer_dirs(root.platforms) if layers is None else layers)
        if d.name != root.layer and (overlay := d / "Source" / relative).is_dir()
    }


def _load_source_root(discovered: Discovered, root: _SourceRoot) -> None:
    layers = _layer_dirs(root.platforms)
    for file in _walk(root.path):
        if file.name.endswith(MODULE_SUFFIX):
            name = file.name.removesuffix(MODULE_SUFFIX)
            rules = _load_rules_class(file, (ModuleRules, ThirdPartyRules))
            if issubclass(rules, ThirdPartyRules):
                _check_package_info(file, rules)
            _check_unique(discovered.modules, name, file)
            kind = _module_kind(file, root.path, rules)
            discovered.modules[name] = ModuleDescriptor(
                name, kind, file, rules, layer=root.layer, overlays=_overlays(file.parent, root, layers)
            )
        elif file.name.endswith(TARGET_SUFFIX):
            name = file.name.removesuffix(TARGET_SUFFIX)
            rules = _load_rules_class(file, (TargetRules,))
            _check_unique(discovered.targets, name, file)
            discovered.targets[name] = TargetDescriptor(name, file, rules)


def _add_test_modules(modules: dict[str, ModuleDescriptor]) -> None:
    """Register a test module for each module with a ``Tests/`` folder.

    A rules file already found in that folder becomes the test module; otherwise one with default
    rules is made up, as if ``Tests/<Module>Tests.module.py`` held an empty ``ModuleRules``. Tests
    only in an overlay are an error, since they would have no module to belong to.
    """
    by_dir = {m.dir: m for m in modules.values()}
    for module in list(modules.values()):
        tests_dir = module.dir / TESTS_DIR
        if module.is_third_party or module.tested_module is not None:
            continue
        if not tests_dir.is_dir():
            for overlay in module.overlays.values():
                if (overlay / TESTS_DIR).is_dir():
                    raise DiscoveryError(
                        f"{overlay / TESTS_DIR}: '{module.name}' has no {TESTS_DIR}/ folder for these tests to overlay"
                    )
            continue
        name = f"{module.name}Tests"
        if (existing := by_dir.get(tests_dir)) is not None:
            if existing.name != name or existing.is_third_party:
                raise DiscoveryError(
                    f"{existing.rules_file}: the tests of '{module.name}' must be a module named '{name}'"
                )
            modules[name] = dataclasses.replace(existing, kind=ModuleKind.Test, tested_module=module.name)
            continue
        rules_file = tests_dir / f"{name}{MODULE_SUFFIX}"
        _check_unique(modules, name, rules_file)
        rules_class = type(name, (ModuleRules,), {"__module__": __name__})
        overlays = {
            layer: overlay / TESTS_DIR for layer, overlay in module.overlays.items() if (overlay / TESTS_DIR).is_dir()
        }
        modules[name] = ModuleDescriptor(
            name,
            ModuleKind.Test,
            rules_file,
            rules_class,
            module.name,
            layer=module.layer,
            overlays=overlays,
        )


def _check_overlays(modules: dict[str, ModuleDescriptor]) -> None:
    """Raise :exc:`DiscoveryError` for an overlay that is the directory of another module."""
    by_dir = {m.dir: m for m in modules.values()}
    for module in modules.values():
        for overlay in module.overlays.values():
            if (other := by_dir.get(overlay)) is not None:
                raise DiscoveryError(
                    f"{other.rules_file}: overlays '{module.name}' ({module.dir}); an overlay has no rules file"
                )


def _walk(root: Path) -> Iterator[Path]:
    if not root.is_dir():
        return
    with os.scandir(root) as scan:
        entries = sorted((Path(e.path), e.is_dir()) for e in scan)
    for entry, is_dir in entries:
        if is_dir:
            if entry.name not in SKIPPED_DIRS and not entry.name.startswith("."):
                yield from _walk(entry)
        elif entry.name.endswith((MODULE_SUFFIX, TARGET_SUFFIX)):
            yield entry


def _module_kind(file: Path, source_root: Path, rules: type) -> ModuleKind:
    if issubclass(rules, ThirdPartyRules):
        return ModuleKind.ThirdParty
    top = file.relative_to(source_root).parts[0]
    match top:
        case "Editor":
            return ModuleKind.Editor
        case "Developer":
            return ModuleKind.Developer
        case "Programs":
            return ModuleKind.Program
        case _:
            return ModuleKind.Runtime


def _check_unique(seen: dict, name: str, file: Path) -> None:
    if name in seen:
        raise DiscoveryError(f"'{name}' is defined twice: {seen[name].rules_file} and {file}")


def _load_rules_class(file: Path, bases: tuple[type, ...]) -> type[Any]:
    """Execute *file* and return the one subclass of *bases* it defines."""
    spec = importlib.util.spec_from_file_location(f"marlin_rules.{file.name.replace('.', '_')}", file)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except Exception as error:
        raise DiscoveryError(f"{file}: {type(error).__name__}: {error}") from error
    found = [
        obj
        for obj in vars(module).values()
        if inspect.isclass(obj) and issubclass(obj, bases) and obj not in bases and obj.__module__ == module.__name__
    ]
    if len(found) != 1:
        expected = " or ".join(b.__name__ for b in bases)
        raise DiscoveryError(f"{file}: expected exactly one {expected} subclass, found {len(found)}")
    return found[0]
