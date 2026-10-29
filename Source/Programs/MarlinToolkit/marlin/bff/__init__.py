"""Generate the FASTBuild .bff from a resolved build graph.

Sources are found by FASTBuild scanning each module directory on every build, so adding, removing or
renaming a source file needs no ``marlin generate``. Unity grouping, and whether a module has C or
resource objects at all, are still decided from the files present at generate time.

Module DLLs shared by every target of a build environment are named ``Marlin-<Module>``; a target
with its own environment names them ``<Target>-<Module>`` so they can sit in the same folder.

MarlinHeaderTool runs as an Exec node per platform, arch and module, before anything that includes the module's
headers compiles; the ``.gen.cpp`` files it writes are scanned when the build reaches them, so they need no
generate either. Each executable that links Core gets ``MarlinModuleTable``, its modules in dependency order.
"""

import json
import math
import os
import re
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from marlin.bff.compdb import CompileCommands, compile_commands
from marlin.bff.solution import SOLUTION_ALIAS, SolutionResult, emit_solution
from marlin.bff.writer import BffWriter, Value
from marlin.build.graph import BuildGraph, ModuleBuild, TargetBuild, compile_spec
from marlin.build.model import LinkMode, Platform, TargetType, Triple
from marlin.build.settings import BuildSettings, CacheMode
from marlin.build.toolchain import LinkSpec, Toolchain
from marlin.build.version import VERSION_HEADER
from marlin.paths import EnginePaths

__all__ = ["SOLUTION_ALIAS", "GenerateResult", "generate"]

CPP_PATTERNS = ["*.cpp", "*.cc", "*.cxx"]
C_PATTERNS = ["*.c"]
OBJC_PATTERNS = ["*.m", "*.mm"]
RC_PATTERNS = ["*.rc"]
SHARED_DLL_PREFIX = "Marlin"
# Every executable and DLL that links Core compiles this file once, routing its operator new and delete to the
# engine heap; Windows gives each binary its own copy of those operators.
OPERATORS_SOURCE = "MemoryOperators.cpp"
OPERATORS_INCLUDE = "Memory/MemoryOperators.inl"
OPERATORS_MODULE = "Core"
HEADER_PATTERNS = ["*.h", "*.hpp", "*.inl"]
MODULE_TABLE_SOURCE = "ModuleTable.gen.cpp"
_RC_DEFINITION = re.compile(r"[A-Za-z_]\w*(=[\w.]*)?")
CACHE_FLAGS = {CacheMode.read: "-cacheread", CacheMode.write: "-cachewrite", CacheMode.readwrite: "-cache"}


@dataclass
class GenerateResult:
    """The files one ``generate`` wrote and the aliases it defined."""

    bff: Path
    manifest: Path
    aliases: list[str]
    solution: SolutionResult | None
    compile_commands: CompileCommands | None


@dataclass
class _TargetNodes:
    """The link node of a target's executable and the other nodes it needs."""

    exe: str
    extra: list[str] = field(default_factory=list)


@dataclass
class _LinkInputs:
    """Libraries and options collected for one link."""

    libraries: list[str | Path] = field(default_factory=list)
    system_libraries: list[str] = field(default_factory=list)
    frameworks: list[str] = field(default_factory=list)
    options: list[str] = field(default_factory=list)

    def add_module(self, module: ModuleBuild) -> None:
        self.system_libraries += module.rules.system_libraries
        self.frameworks += module.rules.frameworks
        self.options += module.rules.link_options


def generate(
    paths: EnginePaths,
    settings: BuildSettings,
    graph: BuildGraph,
    invocation: list[str],
    inputs: dict[str, int | list[int | str]],
) -> GenerateResult:
    """Write the .bff, the version header and the build manifest; return what was written.

    The manifest records the ``marlin generate`` arguments and the inputs it read, so ``marlin build``
    can regenerate with the same arguments when an input changes. The version header is written here
    rather than by a FASTBuild TextFile node: a changed node dirties every node that depends on it,
    while a changed header only rebuilds the sources that include it.
    """
    emitter = _Emitter(paths, settings, graph)
    text = emitter.emit()
    write_if_changed(paths.generated_bff, text)
    write_if_changed(paths.generated_headers / VERSION_HEADER, "\n".join(graph.version.header_lines()) + "\n")
    write_if_changed(paths.generated_headers / OPERATORS_SOURCE, f'#include "{OPERATORS_INCLUDE}"\n')
    if settings.unity.isolate_max > 0 and not paths.unity_isolate_list.is_file():
        # Empty until `marlin build` lists the sources edited lately.
        write_if_changed(paths.unity_isolate_list, "")
    solution = emitter.solution
    if solution:
        write_if_changed(solution.path.with_suffix(".gta.runsettings"), _test_adapter_settings(graph))
    commands = compile_commands(paths, settings, graph) if settings.compile_commands.enabled else None
    if commands:
        for path, content in commands.files.items():
            write_if_changed(path, content)
    projects = [str(p) for p in solution.projects] if solution else _previous_projects(paths)
    if solution:
        _remove_stale_projects(paths, solution.projects)
    manifest = {
        "fbuild_args": fbuild_args(settings),
        "env": fbuild_env(settings),
        "version": graph.version.to_dict(),
        "aliases": emitter.aliases,
        "tests": [_test_entry(target) for target in graph.targets if target.rules.test_modules],
        "solution": str(solution.path) if solution else None,
        "projects": projects,
        "settings": settings.to_dict(),
        "invocation": invocation,
        "inputs": inputs,
    }
    write_if_changed(paths.generated_manifest, json.dumps(manifest, indent=2) + "\n")
    return GenerateResult(paths.generated_bff, paths.generated_manifest, emitter.aliases, solution, commands)


def _test_entry(target: TargetBuild) -> dict[str, object]:
    """Return what ``marlin test`` needs to know about the test target *target*."""
    return {
        "target": target.name,
        "alias": target.alias,
        "platform": str(target.triple.platform),
        "arch": str(target.triple.arch),
        "configuration": str(target.triple.configuration),
        "executable": str(target.binaries / target.toolchain.exe_name(target.output_name)),
        "modules": [m.name for m in target.test_modules],
    }


def _test_adapter_settings(graph: BuildGraph) -> str:
    """Return the solution settings of Visual Studio's Google Test adapter.

    The test targets take the Google Test options, so Test Explorer lists and runs their tests once it knows which
    executables to ask; it runs them from the engine root, as ``marlin test`` does.
    """
    names = sorted({re.escape(t.toolchain.exe_name(t.output_name)) for t in graph.targets if t.rules.test_modules})
    pattern = f"({'|'.join(names)})$" if names else "^$"
    return "\n".join(
        [
            '<?xml version="1.0" encoding="utf-8"?>',
            "<!-- Generated by `marlin generate`. Do not edit. -->",
            "<RunSettings>",
            "  <GoogleTestAdapterSettings>",
            "    <SolutionSettings>",
            "      <Settings>",
            f"        <TestDiscoveryRegex>{pattern}</TestDiscoveryRegex>",
            "        <WorkingDir>$(SolutionDir)</WorkingDir>",
            "      </Settings>",
            "    </SolutionSettings>",
            "  </GoogleTestAdapterSettings>",
            "</RunSettings>",
            "",
        ]
    )


def _previous_projects(paths: EnginePaths) -> list[str]:
    """Return the project files the last generate wrote; a generate without a solution leaves them in place."""
    if not paths.generated_manifest.is_file():
        return []
    return json.loads(paths.generated_manifest.read_text(encoding="utf-8")).get("projects", [])


def _remove_stale_projects(paths: EnginePaths, projects: list[Path]) -> None:
    """Delete the project files a previous generate wrote that are no longer produced.

    Only .vcxproj files inside the engine root are deleted, whatever a damaged manifest lists.
    """
    if not paths.generated_manifest.is_file():
        return
    previous = json.loads(paths.generated_manifest.read_text(encoding="utf-8")).get("projects", [])
    keep = {str(p) for p in projects}
    root = paths.root.resolve()
    for stale in previous:
        if stale in keep or not stale.endswith(".vcxproj"):
            continue
        for suffix in ("", ".filters"):
            path = paths.resolve(stale + suffix)
            if path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(root):
                path.unlink()


def fbuild_args(settings: BuildSettings) -> list[str]:
    """Return the FASTBuild arguments that *settings* imply for every build."""
    args = []
    if settings.cache.enabled:
        args.append(CACHE_FLAGS[settings.cache.mode])
    if settings.distribution.enabled:
        args.append("-dist")
    return args


def fbuild_env(settings: BuildSettings) -> dict[str, str]:
    """Return the environment variables that *settings* imply for every build."""
    if settings.distribution.enabled and settings.distribution.brokerage_path:
        return {"FASTBUILD_BROKERAGE_PATH": settings.distribution.brokerage_path}
    return {}


def write_if_changed(path: Path, text: str) -> bool:
    """Write *text* to *path* unless it already holds it; return whether it was written.

    Leaving an unchanged file alone keeps its timestamp, so FASTBuild sees no change.
    """
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    return True


class _Emitter:
    """Write the nodes for a build graph into one .bff."""

    def __init__(self, paths: EnginePaths, settings: BuildSettings, graph: BuildGraph) -> None:
        self.paths = paths
        self.settings = settings
        self.graph = graph
        self.w = BffWriter()
        self.aliases: list[str] = []
        self.solution: SolutionResult | None = None
        self._module_nodes: dict[str, list[str]] = {}
        self._copies: set[str] = set()
        self._dll_nodes: dict[str, str] = {}
        self._resource_compilers: set[str] = set()
        self._operator_nodes: set[str] = set()
        self._emitted_modules: set[str] = set()
        self._link_nodes: dict[str, list[str]] = {}

    def emit(self) -> str:
        w = self.w
        w.comment("Generated by `marlin generate`. Do not edit; edit Build/BuildSettings.toml or the rules files.")
        w.line()
        self._settings()
        for toolchain in self.graph.toolchains.values():
            self._compiler(toolchain)

        toolchains = {m.node: t.toolchain for t in self.graph.targets for m in t.modules}
        toolchains |= {m.node: self.graph.toolchain_for(m.triple) for m in self.graph.standalone}
        header_tool = self.graph.header_tool
        if header_tool is not None:
            for module in header_tool.modules:
                self._module(module, toolchains[module.node])
            self._target(header_tool)
            self._header_tool_runs(header_tool)
        for module in self.graph.module_builds:
            self._module(module, toolchains[module.node])
        for target in self.graph.targets:
            if target is not header_tool:
                self._target(target)
        self._target_aliases()
        self._aggregate_aliases()
        if self.paths.host_is_windows and self.settings.solution.enabled and self.graph.targets:
            self.solution = emit_solution(w, self.paths, self.settings, self.graph)
        return w.text()

    def _settings(self) -> None:
        props: dict[str, Value | None] = {}
        cache = self.settings.cache
        if cache.enabled:
            props["CachePath"] = str(self.paths.resolve(cache.path))
            if cache.mount_point:
                props["CachePathMountPoint"] = cache.mount_point
        dist = self.settings.distribution
        if dist.enabled and dist.workers:
            props["Workers"] = list(dist.workers)
        self.w.function("Settings", None, props)

    def _compiler(self, toolchain: Toolchain) -> None:
        self.w.comment(toolchain.version)
        self.w.function(
            "Compiler",
            toolchain.name,
            {
                "Executable": toolchain.compiler,
                "ExtraFiles": toolchain.compiler_extra_files or None,
                "CompilerFamily": toolchain.compiler_family,
                "AllowDistribution": self.settings.distribution.enabled,
            },
        )

    def _resource_compiler(self, toolchain: Toolchain) -> str:
        """Declare the toolchain's resource compiler on first use; return its node name.

        It is a custom compiler, which FASTBuild neither caches nor distributes, and the headers an
        .rc file includes are not tracked. Resource scripts are few and small, so that is acceptable.
        """
        name = f"{toolchain.name}-RC"
        if name not in self._resource_compilers:
            self._resource_compilers.add(name)
            self.w.function("Compiler", name, {"Executable": toolchain.resource_compiler, "CompilerFamily": "custom"})
        return name

    def _definitions_header(self, module: ModuleBuild) -> str:
        """Declare the TextFile node of the module's force-included Definitions.h; return its name.

        A header avoids quoting values such as ``__attribute__((visibility("default")))`` on the
        command line. FASTBuild writes it at build time, so deleting Intermediate/ needs no generate.
        """
        node = f"{module.node}-Definitions"
        self.w.function(
            "TextFile",
            node,
            {
                "TextFileOutput": module.intermediate / "Definitions.h",
                "TextFileInputStrings": module.definitions_header(),
            },
        )
        return node

    @staticmethod
    def _scan(
        prefix: str, module: ModuleBuild, patterns: list[str], excluded: Iterable[Path] = ()
    ) -> dict[str, Value | None]:
        """Return the ``<prefix>Path``, ``<prefix>Pattern`` and exclusion properties for *module*.

        Every root of the module is scanned. Other platforms' folders are excluded by patterns
        anchored at each root, so an engine checked out under a folder named after a platform still
        builds. Sources that a higher root replaces are excluded along with *excluded*.
        """
        sep = os.sep
        platform_dirs = [
            pattern
            for root in module.roots
            for other in Platform
            if other is not module.triple.platform
            for pattern in (f"{root}{sep}{other}{sep}*", f"{root}{sep}*{sep}{other}{sep}*")
        ]
        return {
            f"{prefix}Path": [f"{root}{sep}" for root in module.roots],
            f"{prefix}Pattern": patterns,
            f"{prefix}ExcludePath": [f"{d}{sep}" for d in module.nested_module_dirs] or None,
            f"{prefix}ExcludePattern": platform_dirs,
            f"{prefix}ExcludedFiles": sorted({*module.overridden_sources, *excluded}) or None,
        }

    @staticmethod
    def _header_tool_node(triple: Triple, module: str) -> str:
        return f"HeaderTool-{triple.platform}-{triple.arch}-{module}"

    def _header_tool_runs(self, header_tool: TargetBuild) -> None:
        """Declare a header tool run for each module with generated code, once per platform and arch.

        A run is redone when a header or source of the module changes, or the tool itself; it rewrites
        only the outputs whose text changed.
        """
        executable = header_tool.binaries / header_tool.toolchain.exe_name(header_tool.output_name)
        declared: set[str] = set()
        for module in self.graph.module_builds:
            if module.generated is None:
                continue
            node = self._header_tool_node(module.triple, module.name)
            if node in declared:
                continue
            declared.add(node)
            manifest = module.generated / f"{module.name}.manifest"
            arguments = ["--module", module.name, "--output", _quote(module.generated), "--manifest", _quote(manifest)]
            for root in module.roots:
                arguments += ["--root", _quote(root)]
            for nested in module.nested_module_dirs:
                arguments += ["--exclude", _quote(nested)]
            for other in Platform:
                if other is not module.triple.platform:
                    arguments += ["--skip", str(other)]
            self.w.function(
                "Exec",
                node,
                {
                    "ExecExecutable": executable,
                    "ExecArguments": " ".join(arguments),
                    "ExecOutput": manifest,
                    "ExecInputPath": [f"{root}{os.sep}" for root in module.roots],
                    "ExecInputPattern": [*HEADER_PATTERNS, *CPP_PATTERNS],
                    "ExecInputExcludePath": [f"{d}{os.sep}" for d in module.nested_module_dirs] or None,
                },
            )

    def _generated_dependencies(self, module: ModuleBuild) -> list[str]:
        """Return the header tool runs that must finish before *module* compiles."""
        return [self._header_tool_node(module.triple, name) for name in module.generated_dependencies]

    def _module(self, module: ModuleBuild, toolchain: Toolchain) -> None:
        """Declare the object lists of *module* and an alias that compiles them.

        With a PCH, Definitions.h stays force-included: FASTBuild drops ``-include-pch`` when it
        preprocesses for the cache or for distribution. The resource compiler only gets definitions
        that need no command-line quoting, which resource scripts never rely on. Generated sources are
        compiled apart, without the PCH or unity.
        """
        if module.node in self._emitted_modules:
            return
        self._emitted_modules.add(module.node)
        w = self.w
        w.comment(f"Module {module.name} ({module.env_key})")
        nodes: list[str] = []
        definitions = self._definitions_header(module)
        prebuild = [definitions, *self._generated_dependencies(module)]
        input_root = f"{module.descriptor.dir}{os.sep}"
        force_includes = [module.intermediate / "Definitions.h"]
        cpp_spec = compile_spec(module, self.settings, "c++", force_includes)
        objc = [*OBJC_PATTERNS] if module.triple.platform is Platform.macOS else []
        unity_inputs = [
            s
            for s in module.cpp_sources
            if s.suffix not in (".m", ".mm") and not (module.unity_exclude and s.resolve() in module.unity_exclude)
        ]
        if module.cpp_sources:
            props: dict[str, Value | None] = {}
            if module.unity and unity_inputs:
                unity = f"{module.node}-Unity"
                w.function(
                    "Unity",
                    unity,
                    {
                        **self._scan("UnityInput", module, CPP_PATTERNS, module.unity_exclude),
                        "UnityOutputPath": module.intermediate / "Unity",
                        "UnityOutputPattern": f"{module.name}.Unity*.cpp",
                        "UnityNumFiles": math.ceil(len(unity_inputs) / self.settings.unity.files_per_unit),
                        "UnityInputIsolateListFile": (
                            self.paths.unity_isolate_list if self.settings.unity.isolate_max > 0 else None
                        ),
                    },
                )
                props["CompilerInputUnity"] = unity
                if module.unity_exclude:
                    props["CompilerInputFiles"] = sorted(module.unity_exclude)
                    props["CompilerInputFilesRoot"] = input_root
                if objc:
                    props |= self._scan("CompilerInput", module, objc)
            else:
                props |= self._scan("CompilerInput", module, [*CPP_PATTERNS, *objc])
            options = toolchain.compile_options(cpp_spec)
            if module.pch:
                pch_output = module.intermediate / f"{module.pch.stem}{toolchain.pch_ext}"
                props["PCHInputFile"] = module.pch
                props["PCHOutputFile"] = pch_output
                props["PCHOptions"] = " ".join(toolchain.pch_create_options(cpp_spec))
                cpp_spec.pch = pch_output
                options = toolchain.compile_options(cpp_spec)
            node = f"{module.node}-Cpp"
            w.function(
                "ObjectList",
                node,
                {
                    "Compiler": toolchain.name,
                    "CompilerOptions": " ".join(options),
                    "CompilerOutputPath": module.intermediate,
                    "CompilerOutputExtension": toolchain.obj_ext,
                    "PreBuildDependencies": prebuild,
                    **props,
                },
            )
            nodes.append(node)
        if module.c_sources:
            node = f"{module.node}-C"
            w.function(
                "ObjectList",
                node,
                {
                    "Compiler": toolchain.name,
                    "CompilerOptions": " ".join(
                        toolchain.compile_options(compile_spec(module, self.settings, "c", force_includes))
                    ),
                    "CompilerOutputPath": module.intermediate,
                    "CompilerOutputExtension": toolchain.obj_ext,
                    "PreBuildDependencies": prebuild,
                    **self._scan("CompilerInput", module, C_PATTERNS),
                },
            )
            nodes.append(node)
        if module.generated is not None:
            node = f"{module.node}-Generated"
            generated_spec = compile_spec(module, self.settings, "c++", force_includes)
            w.function(
                "ObjectList",
                node,
                {
                    "Compiler": toolchain.name,
                    "CompilerOptions": " ".join(toolchain.compile_options(generated_spec)),
                    "CompilerOutputPath": module.intermediate / "Generated",
                    "CompilerOutputExtension": toolchain.obj_ext,
                    "CompilerInputPath": f"{module.generated / 'Source'}{os.sep}",
                    "CompilerInputPattern": ["*.gen.cpp"],
                    "PreBuildDependencies": prebuild,
                },
            )
            nodes.append(node)
        if module.rc_sources:
            node = f"{module.node}-Rc"
            definitions = [d for d in module.definitions if _RC_DEFINITION.fullmatch(d)]
            w.function(
                "ObjectList",
                node,
                {
                    "Compiler": self._resource_compiler(toolchain),
                    "CompilerOptions": " ".join(toolchain.resource_options(module.include_paths, definitions)),
                    "CompilerOutputPath": module.intermediate,
                    "CompilerOutputExtension": toolchain.res_ext,
                    **self._scan("CompilerInput", module, RC_PATTERNS),
                },
            )
            nodes.append(node)
        self._module_nodes[module.node] = nodes
        w.function("Alias", module.node, {"Targets": nodes})

    def _target(self, target: TargetBuild) -> None:
        """Declare the link nodes of *target* and its alias."""
        self.w.comment(f"Target {target.alias} ({target.target_type}, {target.ctx.link_mode})")
        if target.ctx.link_mode is LinkMode.Monolithic:
            nodes = self._link_monolithic(target)
        else:
            nodes = self._link_modular(target)
        nodes.extra += self._runtime_copies(target)
        self._link_nodes[target.alias] = [nodes.exe, *nodes.extra]

    def _target_aliases(self) -> None:
        """Declare the alias of each target, its companions' first: FASTBuild needs a node declared before it is named."""
        by_alias = {t.alias: t for t in self.graph.targets}
        declared: set[str] = set()

        def declare(target: TargetBuild) -> None:
            if target.alias in declared:
                return
            declared.add(target.alias)
            nodes = list(self._link_nodes[target.alias])
            for companion in target.rules.companion_targets:
                alias = f"{companion}-{target.triple}"
                if alias in by_alias:
                    declare(by_alias[alias])
                    nodes.append(alias)
            self.w.function("Alias", target.alias, {"Targets": nodes})
            self.aliases.append(target.alias)

        for target in self.graph.targets:
            declare(target)

    def _link_spec(
        self, target: TargetBuild, shared: bool, output_stem: str, inputs: _LinkInputs, import_dir: Path
    ) -> LinkSpec:
        """Return how *target* links *output_stem*, an executable or a shared library."""
        configuration = self.settings.configurations[target.triple.configuration]
        toolchain = target.toolchain
        import_lib = toolchain.import_lib_name(output_stem) if shared else None
        debug_file = toolchain.debug_file_name(output_stem)
        console = target.rules.console
        if console is None:
            console = target.target_type in (TargetType.Program, TargetType.Server)
        return LinkSpec(
            shared=shared,
            console=not shared and console,
            optimize=configuration.optimize,
            lto=configuration.lto,
            debug_info=configuration.debug_info,
            sanitizers=configuration.sanitizers,
            system_libraries=list(dict.fromkeys(inputs.system_libraries)),
            frameworks=list(dict.fromkeys(inputs.frameworks)),
            import_library=import_dir / import_lib if import_lib else None,
            pdb=target.binaries / debug_file if debug_file and configuration.debug_info else None,
            install_name=toolchain.shared_lib_name(output_stem) if shared else None,
            extra=list(dict.fromkeys(inputs.options)),
        )

    @staticmethod
    def _add_third_party(inputs: _LinkInputs, target: TargetBuild, names: list[str]) -> None:
        """Add the libraries and options of the third-party modules *names* to *inputs*."""
        for name in names:
            third_party = target.third_party[name]
            inputs.libraries += third_party.static_libraries
            inputs.libraries += [link for _, link in third_party.shared_libraries if link is not None]
            inputs.libraries += [runtime for runtime, link in third_party.shared_libraries if link is None]
            inputs.system_libraries += third_party.system_libraries
            inputs.frameworks += third_party.frameworks
            inputs.options += third_party.link_options

    def _operators(self, module: ModuleBuild, toolchain: Toolchain) -> list[str]:
        """Return the node compiling the operators source with *module*'s options, if *module* sees Core."""
        if module.name != OPERATORS_MODULE and OPERATORS_MODULE not in module.visible_modules:
            return []
        node = f"{module.node}-Operators"
        if node not in self._operator_nodes:
            self._operator_nodes.add(node)
            spec = compile_spec(module, self.settings, "c++", [module.intermediate / "Definitions.h"])
            self.w.function(
                "ObjectList",
                node,
                {
                    "Compiler": toolchain.name,
                    "CompilerOptions": " ".join(toolchain.compile_options(spec)),
                    "CompilerInputFiles": [self.paths.generated_headers / OPERATORS_SOURCE],
                    "CompilerOutputPath": module.intermediate / "Operators",
                    "CompilerOutputExtension": toolchain.obj_ext,
                    "PreBuildDependencies": [f"{module.node}-Definitions"],
                },
            )
        return [node]

    def _module_table(self, target: TargetBuild) -> list[str]:
        """Return the node compiling *target*'s module table, if its launch module sees Core.

        A monolithic table points at every Register_<Module>; a modular one names the shared library of each
        module but the launch module, which registration loads and searches.
        """
        launch = target.launch_module
        if launch.generated is None:
            return []
        entries: list[str] = []
        declarations: list[str] = []
        for module in target.modules:
            if module.generated is None:
                continue
            if self._in_executable(target, module):
                declarations.append(f'extern "C" void Register_{module.name}();')
                entries.append(f'    {{"{module.name}", &Register_{module.name}, nullptr}},')
            else:
                stem = f"{SHARED_DLL_PREFIX if module.shared else target.name}-{module.name}"
                library = target.toolchain.shared_lib_name(stem)
                entries.append(f'    {{"{module.name}", nullptr, "{library}"}},')
        source = target.intermediate / MODULE_TABLE_SOURCE
        lines = [
            "// Generated by `marlin generate`. Do not edit.",
            '#include "Reflection/ModuleTable.h"',
            "",
            *declarations,
            "",
            'extern "C" const ::Marlin::ModuleTableEntry MarlinModuleTable[] = {',
            *entries,
            "};",
            f'extern "C" const ::Marlin::usize MarlinModuleTableCount = {len(entries)};',
        ]
        write_if_changed(source, "\n".join(lines) + "\n")
        node = f"{target.alias}-ModuleTable"
        spec = compile_spec(launch, self.settings, "c++", [launch.intermediate / "Definitions.h"])
        self.w.function(
            "ObjectList",
            node,
            {
                "Compiler": target.toolchain.name,
                "CompilerOptions": " ".join(target.toolchain.compile_options(spec)),
                "CompilerInputFiles": [source],
                "CompilerOutputPath": target.intermediate / "ModuleTable",
                "CompilerOutputExtension": target.toolchain.obj_ext,
                "PreBuildDependencies": [f"{launch.node}-Definitions", *self._generated_dependencies(launch)],
            },
        )
        return [node]

    def _link_monolithic(self, target: TargetBuild) -> _TargetNodes:
        """Link every module of *target* into one executable."""
        inputs = _LinkInputs(libraries=[n for m in target.modules for n in self._module_nodes[m.node]])
        inputs.libraries += self._module_table(target)
        core = next((m for m in target.modules if m.name == OPERATORS_MODULE), None)
        if core is not None:
            inputs.libraries += self._operators(core, target.toolchain)
        for module in target.modules:
            inputs.add_module(module)
        self._add_third_party(inputs, target, list(target.third_party))
        inputs.options += target.rules.link_options
        exe = self._link(target, "Executable", target.output_name, inputs, f"{target.alias}-Exe")
        return _TargetNodes(exe)

    @staticmethod
    def _in_executable(target: TargetBuild, module: ModuleBuild) -> bool:
        """Return whether *module* links into *target*'s executable rather than a DLL of its own.

        Test modules do, as nothing depends on them; their symbols then sit in the executable's PDB, which is
        where test adapters look for each test's source.
        """
        return target.ctx.is_monolithic or module is target.launch_module or module.descriptor.tested_module is not None

    def _link_modular(self, target: TargetBuild) -> _TargetNodes:
        """Link every module of *target* into its own DLL, except the launch module and the test modules.

        The DLLs are shared by every target built from the same build environment.
        """
        modules = {m.name: m for m in target.modules}
        launch = target.launch_module
        dll_nodes = [self._module_dll(target, m, modules) for m in target.modules if not self._in_executable(target, m)]
        inputs = self._module_link_inputs(target, launch, modules)
        for test in target.test_modules:
            test_inputs = self._module_link_inputs(target, test, modules, operators=False)
            inputs.libraries += test_inputs.libraries
            inputs.system_libraries += test_inputs.system_libraries
            inputs.frameworks += test_inputs.frameworks
            inputs.options += test_inputs.options
        inputs.libraries = list(dict.fromkeys(inputs.libraries))
        inputs.libraries += self._module_table(target)
        inputs.options += target.rules.link_options
        exe = self._link(target, "Executable", target.output_name, inputs, f"{target.alias}-Exe")
        return _TargetNodes(exe, dll_nodes)

    def _module_dll(self, target: TargetBuild, module: ModuleBuild, modules: dict[str, ModuleBuild]) -> str:
        """Declare the DLL of *module* on first use; return its node name."""
        if module.node not in self._dll_nodes:
            stem = f"{SHARED_DLL_PREFIX if module.shared else target.name}-{module.name}"
            inputs = self._module_link_inputs(target, module, modules)
            self._dll_nodes[module.node] = self._link(
                target, "DLL", stem, inputs, f"{module.node}-DLL", import_dir=module.intermediate
            )
        return self._dll_nodes[module.node]

    def _module_link_inputs(
        self, target: TargetBuild, module: ModuleBuild, modules: dict[str, ModuleBuild], operators: bool = True
    ) -> _LinkInputs:
        """Return the link inputs of *module*: its objects and the DLLs of the modules it sees.

        Dependencies come first in ``target.modules``, so their DLL nodes already exist. *operators* is false for a
        module joining a binary that already has them.
        """
        own_operators = self._operators(module, target.toolchain) if operators else []
        inputs = _LinkInputs(libraries=[*self._module_nodes[module.node], *own_operators])
        inputs.libraries += [self._dll_nodes[modules[name].node] for name in module.visible_modules]
        inputs.add_module(module)
        self._add_third_party(inputs, target, module.visible_third_party)
        return inputs

    def _link(
        self,
        target: TargetBuild,
        function: str,
        stem: str,
        inputs: _LinkInputs,
        node: str,
        import_dir: Path | None = None,
    ) -> str:
        """Declare a link node of type *function* (``Executable`` or ``DLL``); return its name."""
        toolchain = target.toolchain
        shared = function == "DLL"
        spec = self._link_spec(target, shared, stem, inputs, import_dir or target.intermediate)
        output = target.binaries / (toolchain.shared_lib_name(stem) if shared else toolchain.exe_name(stem))
        self.w.function(
            function,
            node,
            {
                "Linker": toolchain.linker,
                "LinkerType": toolchain.linker_type,
                "LinkerOptions": " ".join(toolchain.link_options(spec)),
                "LinkerOutput": output,
                "Libraries": inputs.libraries,
            },
        )
        return node

    def _runtime_copies(self, target: TargetBuild) -> list[str]:
        """Declare copies of the third-party and sanitizer shared libraries next to *target*; return their nodes."""
        sanitizers = self.settings.configurations[target.triple.configuration].sanitizers
        runtimes = [
            runtime for third_party in target.third_party.values() for runtime, _ in third_party.shared_libraries
        ]
        runtimes += target.toolchain.sanitizer_runtimes(sanitizers)
        nodes = []
        for runtime in runtimes:
            dest = target.binaries / runtime.name
            node = f"Copy-{dest}"
            if node not in self._copies:
                self._copies.add(node)
                self.w.function("Copy", node, {"Source": runtime, "Dest": dest})
            nodes.append(node)
        return nodes

    def _aggregate_aliases(self) -> None:
        """Declare ``All-<triple>`` for each combination and ``all`` for every target."""
        groups: dict[str, list[str]] = {}
        for target in self.graph.targets:
            groups.setdefault(f"All-{target.triple}", []).append(target.alias)
        for name, targets in sorted(groups.items()):
            self.w.function("Alias", name, {"Targets": targets})
        if self.aliases:
            self.w.function("Alias", "all", {"Targets": list(self.aliases)})


def _quote(path: Path) -> str:
    return f'"{path}"'
