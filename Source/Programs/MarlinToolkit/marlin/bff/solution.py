"""Generate the Visual Studio solution and a makefile project per module and per target.

Each project is written next to its rules file. A solution configuration is a build configuration
and a platform, such as "Development | Windows"; it builds the editor, the game and the programs
alike, since they share compiled modules. Building the solution builds the target projects, and
building a module project compiles that module alone.

Projects build through ``marlin build``, which regenerates first when an input changed. IntelliSense
uses each module's own include paths and definitions, and parses in clang mode when Visual Studio
has the ClangCL toolset ("C++ Clang tools for Windows"); otherwise it applies MSVC rules, and is given
the integer type macros clang predefines, which CoreTypes.h is built on.
"""

import sys
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path

from marlin.bff.writer import BffWriter, StructRef, Value
from marlin.build.discovery import ModuleDescriptor, ModuleKind, nested_dirs
from marlin.build.graph import BuildGraph, ModuleBuild, TargetBuild
from marlin.build.model import Configuration, Platform, TargetType
from marlin.build.platforms import TRAITS
from marlin.build.settings import BuildSettings
from marlin.build.visualstudio import VisualStudio, resolve
from marlin.paths import EnginePaths

SOLUTION_ALIAS = "solution"
ALLOWED_EXTENSIONS = [
    "*.h",
    "*.hpp",
    "*.hxx",
    "*.inl",
    "*.cpp",
    "*.cc",
    "*.cxx",
    "*.c",
    "*.m",
    "*.mm",
    "*.rc",
    "*.py",
    "*.natvis",
]
# Shaders and scripts: the engine's Language/ folder and each sample's, shown but not built by projects.
LANGUAGE_EXTENSIONS = ["*.mshader", "*.mscript", "*.mcommon"]
# What a sample project shows besides its language files: its asset import settings and the text of its configuration.
# Only these extensions, so the large source assets under Content are never listed.
SAMPLE_EXTENSIONS = [*LANGUAGE_EXTENSIONS, "*.masset", "*.mfiles", "*.mtext"]
SAMPLE_FOLDERS = ["Language", "Config", "Content"]

type _Key = tuple[Platform, Configuration]

# What clang predefines for the 64-bit Windows targets, for IntelliSense in MSVC mode.
CLANG_TYPE_MACROS = [
    "__INT8_TYPE__=signed char",
    "__INT16_TYPE__=short",
    "__INT32_TYPE__=int",
    "__INT64_TYPE__=long long",
    "__UINT8_TYPE__=unsigned char",
    "__UINT16_TYPE__=unsigned short",
    "__UINT32_TYPE__=unsigned int",
    "__UINT64_TYPE__=unsigned long long",
    "__SIZE_TYPE__=unsigned long long",
    "__PTRDIFF_TYPE__=long long",
    "__INTPTR_TYPE__=long long",
    "__UINTPTR_TYPE__=unsigned long long",
]


@dataclass
class _Project:
    """A project to write.

    *targets* holds a target project's builds by platform and configuration. *module* names the
    module whose builds provide IntelliSense and, for a module project, the build command.
    """

    name: str
    output: Path
    folder: str
    base: Path
    input_dirs: list[Path] = field(default_factory=list)
    exclude_dirs: list[Path] = field(default_factory=list)
    files: list[Path] = field(default_factory=list)
    targets: dict[tuple[Platform, Configuration], TargetBuild] = field(default_factory=dict)
    module: str | None = None
    extensions: list[str] = field(default_factory=lambda: ALLOWED_EXTENSIONS)


@dataclass
class SolutionResult:
    """The solution written; *intellisense* is ``"clang"`` or ``"MSVC"``."""

    path: Path
    projects: list[Path]
    visual_studio: VisualStudio
    intellisense: str


def emit_solution(w: BffWriter, paths: EnginePaths, settings: BuildSettings, graph: BuildGraph) -> SolutionResult:
    """Declare the project and solution nodes for *graph*; return what they will write.

    Each project keeps Visual Studio's own state under Intermediate/ProjectFiles instead of a folder
    in the source tree.
    """
    visual_studio = resolve(settings.solution.visual_studio)
    module_builds = _module_builds_by_key(graph)
    projects = [*_plan_projects(graph), *_language_projects(paths)]
    platforms = sorted({t.triple.platform for t in graph.targets}, key=list(Platform).index)
    platforms = [p for p in platforms if _vs_platform(graph, p) is not None]
    configurations = sorted({t.triple.configuration for t in graph.targets}, key=list(Configuration).index)
    marlin = f'cd /d "{paths.root}" && "{sys.executable}" -m marlin build --ide'
    standard = {"c++20": "/std:c++20", "c++23": "/std:c++latest"}.get(settings.cpp_standard, "/std:c++latest")
    toolsets = {
        p: "ClangCL"
        for p in platforms
        if p is Platform.Windows and visual_studio.has_platform_toolset(_vs_platform(graph, p) or "", "ClangCL")
    }

    node_names: dict[str, str] = {}
    for project in projects:
        configs: list[tuple[str, Mapping[str, Value | None]]] = []
        for platform in platforms:
            for configuration in configurations:
                key = (platform, configuration)
                project_intermediate = (
                    paths.intermediate / "ProjectFiles" / project.name / f"{platform}_{configuration}"
                )
                props: dict[str, Value | None] = {
                    "Platform": _vs_platform(graph, platform),
                    "Config": str(configuration),
                    "OutputDirectory": project_intermediate,
                    "IntermediateDirectory": project_intermediate,
                    "PlatformToolset": toolsets.get(platform),
                }
                target = project.targets.get(key)
                module = module_builds.get((project.module, *key)) if project.module else None
                if target is not None:
                    exe = target.binaries / target.toolchain.exe_name(target.output_name)
                    props |= _commands(f"{marlin} {target.alias}")
                    props |= {
                        "Output": exe,
                        "OutputDirectory": target.binaries,
                        "IntermediateDirectory": target.intermediate,
                        "LocalDebuggerCommand": exe,
                        "LocalDebuggerWorkingDirectory": target.binaries,
                        "LocalDebuggerCommandArguments": _debug_arguments(paths, settings, target),
                    }
                    module = module or target.launch_module
                elif module is not None and not project.targets:
                    props |= _commands(f"{marlin} {module.node}")
                elif project.module is None and not project.targets:
                    props |= _commands(f"echo {project.name} has nothing to build")
                else:
                    props |= _commands(f"echo {project.name} is not built in {configuration} ({platform})")
                if module is not None:
                    includes = [*module.include_paths, *module.system_include_paths]
                    definitions = list(module.definitions)
                    if platform is Platform.Windows and platform not in toolsets:
                        definitions += CLANG_TYPE_MACROS
                    props |= {
                        "PreprocessorDefinitions": ";".join(definitions),
                        "IncludeSearchPath": ";".join(str(p) for p in includes),
                        "AdditionalOptions": standard,
                    }
                configs.append((f"Config_{len(configs)}", props))
        node = f"VCXProj-{project.name}"
        w.function(
            "VCXProject",
            node,
            {
                "ProjectOutput": project.output,
                "ProjectBasePath": [f"{d}\\" for d in dict.fromkeys([project.base, *project.input_dirs])],
                "ProjectInputPaths": [f"{d}\\" for d in project.input_dirs],
                "ProjectInputPathsExclude": [f"{d}\\" for d in project.exclude_dirs] or None,
                "ProjectAllowedFileExtensions": project.extensions,
                "ProjectFiles": list(project.files) or None,
                "ProjectConfigs": [StructRef(n) for n, _ in configs],
            },
            structs=configs,
        )
        node_names[project.name] = node

    target_projects = [p for p in projects if p.targets]
    solution_structs: list[tuple[str, Mapping[str, Value | None]]] = []
    for platform in platforms:
        for configuration in configurations:
            build_projects = [node_names[p.name] for p in target_projects if (platform, configuration) in p.targets]
            solution_structs.append(
                (
                    f"SolutionConfig_{len(solution_structs)}",
                    {
                        "Platform": _vs_platform(graph, platform),
                        "Config": str(configuration),
                        "SolutionPlatform": str(platform),
                        "SolutionConfig": str(configuration),
                        "SolutionBuildProject": build_projects,
                    },
                )
            )
    folders: dict[str, list[str]] = {}
    for project in projects:
        folders.setdefault(project.folder, []).append(node_names[project.name])
    folder_structs = [(f"Folder_{i}", {"Path": f, "Projects": p}) for i, (f, p) in enumerate(sorted(folders.items()))]
    solution = paths.root / f"{settings.solution.name}.sln"
    w.function(
        "VSSolution",
        SOLUTION_ALIAS,
        {
            "SolutionOutput": solution,
            "SolutionProjects": list(node_names.values()),
            "SolutionConfigs": [StructRef(n) for n, _ in solution_structs],
            "SolutionFolders": [StructRef(n) for n, _ in folder_structs],
            "SolutionVisualStudioVersion": visual_studio.version,
            "SolutionMinimumVisualStudioVersion": "10.0.40219.1",
        },
        structs=[*solution_structs, *folder_structs],
    )
    intellisense = "clang" if Platform.Windows in toolsets else "MSVC"
    return SolutionResult(solution, [p.output for p in projects], visual_studio, intellisense)


def _plan_projects(graph: BuildGraph) -> list[_Project]:
    """Return a project per target and per module.

    A target with a module of the same name, as programs usually have, gets one project for both.
    """
    targets: dict[str, list[TargetBuild]] = {}
    for target in graph.targets:
        targets.setdefault(target.name, []).append(target)
    modules = {m.name: m.descriptor for m in graph.module_builds}
    nested = nested_dirs(d for m in modules.values() for d in _dirs(m))

    projects: list[_Project] = []
    for name, builds in targets.items():
        first = builds[0]
        if first.rules.test_modules:
            folder = "Tests"
        elif first.target_type is TargetType.Program:
            folder = "Programs"
        else:
            folder = "Targets"
        rules_dir = first.descriptor.rules_file.parent
        project = _Project(
            name=name,
            output=rules_dir / f"{name}.vcxproj",
            folder=folder,
            base=rules_dir,
            targets={(b.triple.platform, b.triple.configuration): b for b in builds},
        )
        if (merged := modules.pop(name, None)) is not None:
            project.input_dirs = _dirs(merged)
            project.exclude_dirs = [n for d in project.input_dirs for n in nested[d]]
            project.base = merged.dir
            project.output = merged.dir / f"{name}.vcxproj"
            project.module = name
            if first.descriptor.rules_file.parent != merged.dir:
                project.files = [first.descriptor.rules_file]
        else:
            project.files = [first.descriptor.rules_file]
        projects.append(project)

    for name, descriptor in sorted(modules.items()):
        if descriptor.kind is ModuleKind.Test:
            folder = "Tests"
        elif descriptor.kind is ModuleKind.Program:
            folder = "Modules\\Programs"
        else:
            folder = f"Modules\\{descriptor.kind}"
        projects.append(
            _Project(
                name=name,
                output=descriptor.dir / f"{name}.vcxproj",
                folder=folder,
                base=descriptor.dir,
                input_dirs=_dirs(descriptor),
                exclude_dirs=[n for d in _dirs(descriptor) for n in nested[d]],
                module=name,
            )
        )
    return projects


def _language_projects(paths: EnginePaths) -> list[_Project]:
    """Return a project for the engine's shaders and scripts and one for each sample project's."""
    projects: list[_Project] = []
    if (language := paths.root / "Language").is_dir():
        projects.append(
            _Project(
                name="Language",
                output=language / "Language.vcxproj",
                folder="Language",
                base=language,
                input_dirs=[language],
                extensions=LANGUAGE_EXTENSIONS,
            )
        )
    for mproject in sorted((paths.root / "Samples").glob("*/*.mproject")):
        if folders := [d for name in SAMPLE_FOLDERS if (d := mproject.parent / name).is_dir()]:
            projects.append(
                _Project(
                    name=mproject.stem,
                    output=mproject.with_suffix(".vcxproj"),
                    folder="Samples",
                    base=mproject.parent,
                    input_dirs=folders,
                    extensions=SAMPLE_EXTENSIONS,
                    files=[mproject],
                )
            )
    return projects


def _dirs(descriptor: ModuleDescriptor) -> list[Path]:
    """Return a module's directory and its overlays in every layer; a project shows all platforms."""
    return [descriptor.dir, *descriptor.overlays.values()]


def _module_builds_by_key(graph: BuildGraph) -> dict[tuple[str, Platform, Configuration], ModuleBuild]:
    """Return the module build each project shows, preferring the shared build environment."""
    result: dict[tuple[str, Platform, Configuration], ModuleBuild] = {}
    for target in graph.targets:
        key: _Key = (target.triple.platform, target.triple.configuration)
        for module in target.modules:
            current = result.get((module.name, *key))
            if current is None or (module.shared and not current.shared):
                result[(module.name, *key)] = module
    for module in graph.standalone:
        result.setdefault((module.name, module.triple.platform, module.triple.configuration), module)
    return result


def _debug_arguments(paths: EnginePaths, settings: BuildSettings, target: TargetBuild) -> str | None:
    """The command line a target starts with under the IDE's debugger: the solution's debug project for targets that
    open projects."""
    project = settings.solution.debug_project
    opens_projects = target.target_type is not TargetType.Program and not target.rules.test_modules
    if not project or not opens_projects:
        return None
    return f'-Project="{(paths.root / project).resolve()}"'


def _commands(command: str) -> dict[str, Value | None]:
    """Return the build and rebuild command properties for *command*."""
    rebuild = command.replace(" --ide", " --ide --clean")
    return {"ProjectBuildCommand": command, "ProjectRebuildCommand": rebuild}


def _vs_platform(graph: BuildGraph, platform: Platform) -> str | None:
    """Return Visual Studio's name for *platform*, or ``None`` if it has none."""
    arch = next(t.triple.arch for t in graph.targets if t.triple.platform is platform)
    return TRAITS[platform].vs_platforms.get(arch)
