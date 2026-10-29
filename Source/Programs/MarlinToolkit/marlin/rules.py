"""Define the public API of ``*.module.py``, ``*.target.py`` and ``*.tool.py`` rules files.

A rules file defines exactly one subclass of :class:`ModuleRules`, :class:`ThirdPartyRules`,
:class:`TargetRules` or :class:`ToolRules`. The module or target is named after the file up to its first dot, so
``Core.module.py`` defines ``Core``. List attributes can be declared on the class and extended in :meth:`setup` for
the parts that depend on the build combination:

    from marlin.rules import *

    class Core(ModuleRules):
        public_dependencies = ["Fmt"]

        def setup(self, ctx: BuildContext) -> None:
            if ctx.platform is Platform.Windows:
                self.system_libraries += ["dbghelp"]

Calling ``self.unsupported(reason)`` from :meth:`setup` excludes every build combination that needs
the module or target, such as the Arm64 builds of a library that ships no Arm64 binaries.
"""

from dataclasses import dataclass
from pathlib import Path
from typing import ClassVar, NoReturn

from marlin.build.model import Arch, Configuration, LinkMode, Platform, TargetType

__all__ = [
    "Arch",
    "BuildContext",
    "Configuration",
    "LinkMode",
    "ModuleRules",
    "Platform",
    "TargetRules",
    "TargetType",
    "ThirdPartyRules",
    "ToolRules",
    "Unsupported",
]


class Unsupported(Exception):
    """Raised by rules to exclude the current build combination."""


@dataclass(frozen=True)
class BuildContext:
    """The build combination that rules are evaluated for.

    There is no target type: modules are compiled once per combination and shared by the editor,
    the game and the programs, so module rules cannot differ between them. A target's own type is
    ``self.type``.
    """

    platform: Platform
    arch: Arch
    configuration: Configuration
    link_mode: LinkMode

    @property
    def is_shipping(self) -> bool:
        return self.configuration is Configuration.Shipping

    @property
    def is_debug(self) -> bool:
        return self.configuration is Configuration.Debug

    @property
    def is_monolithic(self) -> bool:
        return self.link_mode is LinkMode.Monolithic


class _Rules:
    """Base class of all rules.

    Each instance gets its own copy of the list attributes in *_list_fields*, so extending one in
    :meth:`setup` never affects the instances evaluated for other build combinations.
    """

    _list_fields: ClassVar[tuple[str, ...]] = ()

    def __init__(self, name: str, directory: Path) -> None:
        self.name = name
        self.dir = directory
        for field_name in self._list_fields:
            setattr(self, field_name, list(getattr(type(self), field_name, ())))

    def setup(self, ctx: BuildContext) -> None:
        """Adjust the rules for the build combination *ctx*; override in subclasses."""

    def unsupported(self, reason: str) -> NoReturn:
        """Exclude the current build combination, giving *reason*."""
        raise Unsupported(f"{self.name}: {reason}")


class ModuleRules(_Rules):
    """A C++ module compiled from the sources under its directory.

    Header visibility follows the directory. ``Public/`` is seen by every module that depends on
    this one, ``Internal/`` only by dependents inside the engine, and ``Private/`` by this module
    alone. A module with none of them gets its own directory as a private include path. The sources
    are every .cpp and .c file below the module directory, plus .m and .mm on macOS and .rc on
    Windows, except those in a directory named after another platform.

    Attributes:

    *public_dependencies*, *private_dependencies*
        Modules this one uses; dependents also see the public ones.
    *public_include_paths*, *private_include_paths*
        Extra include paths, relative to the module directory.
    *public_definitions*, *private_definitions*
        Preprocessor definitions, ``NAME`` or ``NAME=VALUE``.
    *system_libraries*
        Link names without prefix or extension, such as ``"dbghelp"``. The C runtime and what it
        needs are linked into every binary by the toolchain.
    *frameworks*
        macOS frameworks, such as ``"Metal"``.
    *compile_options*
        Compiler options for this module's sources, after the engine's own.
    *link_options*
        Linker options for every binary this module is linked into.
    *unity*, *warnings_as_errors*
        ``None`` follows BuildSettings.toml.
    *unity_exclude*
        Sources, relative to the module directory, compiled outside the unity files.
    *pch*
        A header, relative to the module directory, to precompile; ``None`` for no PCH.
    *allow_exceptions*, *allow_rtti*
        Compile with C++ exceptions or RTTI. Only modules under ``Programs/`` may set them, to wrap
        third-party code that needs them; nothing may throw across their interface.
    """

    _list_fields = (
        "public_dependencies",
        "private_dependencies",
        "public_include_paths",
        "private_include_paths",
        "public_definitions",
        "private_definitions",
        "system_libraries",
        "frameworks",
        "unity_exclude",
        "compile_options",
        "link_options",
    )

    public_dependencies: list[str]
    private_dependencies: list[str]
    public_include_paths: list[str]
    private_include_paths: list[str]
    public_definitions: list[str]
    private_definitions: list[str]
    system_libraries: list[str]
    frameworks: list[str]
    compile_options: list[str]
    link_options: list[str]
    unity: bool | None = None
    unity_exclude: list[str]
    pch: str | None = None
    warnings_as_errors: bool | None = None
    allow_exceptions: bool = False
    allow_rtti: bool = False


@dataclass(frozen=True)
class SharedLibrary:
    """A shared library of a third-party module; see :meth:`ThirdPartyRules.add_shared_library`."""

    runtime: str
    link: str | None


class _PackageInfo:
    """Where a checked-in package comes from; every field is required.

    *version* is the upstream release or tag, *url* the page it was taken from and *license* an SPDX
    identifier. They are declared next to the package, never in its folder name.
    """

    version: str = ""
    url: str = ""
    license: str = ""


class ThirdPartyRules(_PackageInfo, _Rules):
    """A prebuilt dependency: any mix of headers, static libraries and shared libraries.

    Paths are relative to the module directory. *link_options* apply to every binary that links the
    library. *version*, *url* and *license* describe the package.
    """

    _list_fields = (
        "public_dependencies",
        "public_include_paths",
        "public_definitions",
        "system_libraries",
        "frameworks",
        "link_options",
        "static_libraries",
        "shared_libraries",
    )

    public_dependencies: list[str]
    public_include_paths: list[str]
    public_definitions: list[str]
    system_libraries: list[str]
    frameworks: list[str]
    link_options: list[str]
    static_libraries: list[str]
    shared_libraries: list[SharedLibrary]

    def add_static_library(self, path: str) -> None:
        """Link the static library *path*."""
        self.static_libraries.append(path)

    def add_shared_library(self, runtime: str, link: str | None = None) -> None:
        """Link a shared library and copy *runtime* next to the binaries.

        *link* is the linker input, such as an import library; it defaults to *runtime*, as is usual
        on Linux and macOS.
        """
        self.shared_libraries.append(SharedLibrary(runtime, link))


class TargetRules(_Rules):
    """An executable: a launch module and everything it depends on.

    Attributes:

    *launch_module*
        The module that provides ``main``; required.
    *extra_modules*
        Modules to build into the target that the launch module does not depend on.
    *definitions*
        Global definitions. A target that sets any compiles its own copy of every module instead
        of sharing them with other targets.
    *link_options*
        Linker options for the executable. Module DLLs are shared between targets, so options for
        them belong in the module's ``link_options``.
    *platforms*, *architectures*, *configurations*
        Restrict the generated combinations; ``None`` allows every valid one.
    *output_name*
        The executable name; ``None`` uses the target name.
    *test_modules*
        Build the test module of every module into this target. A module's tests are the sources in
        its ``Tests/`` folder, built as the module ``<Module>Tests``, which sees all of the module's
        headers. Tests the target cannot hold, such as an editor module's tests in a program, and
        tests that do not support the build combination are left out.
    *console*
        Link the executable as a console program, which opens a terminal for its output when it is not started
        from one; only Windows tells the two apart. ``None`` gives programs and servers a console and the game
        and editor none; set it in :meth:`setup` to choose per configuration.
    *companion_targets*
        Targets of the same build combination that this one starts at run time, such as the asset index daemon.
        Building this target builds them too; they are not linked into it. A companion the combination does not
        have is skipped.
    """

    _list_fields = ("extra_modules", "definitions", "link_options", "companion_targets")

    type: TargetType = TargetType.Game
    launch_module: str = ""
    extra_modules: list[str]
    definitions: list[str]
    link_options: list[str]
    companion_targets: list[str]
    platforms: set[Platform] | None = None
    architectures: set[Arch] | None = None
    configurations: set[Configuration] | None = None
    output_name: str | None = None
    test_modules: bool = False
    console: bool | None = None


class ToolRules(_PackageInfo, _Rules):
    """Prebuilt programs checked in under ``Extras/<Name>/`` and described by ``<Name>.tool.py`` there.

    The programs of each host live in ``Extras/<Name>/<Platform>/<Arch>/``, and the license text next to the rules
    file. They are neither built nor linked; the toolkit finds the host's build with
    :meth:`marlin.paths.EnginePaths.tool_executable`. *version*, *url* and *license* describe the package.
    """
