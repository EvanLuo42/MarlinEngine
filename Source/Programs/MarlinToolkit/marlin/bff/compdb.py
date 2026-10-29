"""Generate compile_commands.json for clangd, CLion and other tools that read one.

The database is written from the build graph rather than by ``FBuild -compdb``, which lists the
unity files; tools need an entry per source file. Each source appears once, from the build
environment closest to ``compile_commands.configuration``.

Definitions are force-included from a header written here, since the build's own Definitions.h
exists only after a build. The options a module's sources share go into a response file per module
and language, which keeps the database small; clangd expands response files itself.
"""

import json
from dataclasses import dataclass
from pathlib import Path

from marlin.build.graph import C_EXTENSIONS, BuildGraph, ModuleBuild, compile_spec
from marlin.build.settings import BuildSettings
from marlin.build.toolchain import Toolchain
from marlin.paths import EnginePaths

FILE_NAME = "compile_commands.json"


@dataclass
class CompileCommands:
    """A compilation database and the files it needs.

    *configurations* lists the configurations the entries come from; it differs from the configured
    one when generate was restricted to others. *files* maps every file to write, including the
    headers and response files, to its content.
    """

    path: Path
    sources: int
    configurations: list[str]
    files: dict[Path, str]


def compile_commands(paths: EnginePaths, settings: BuildSettings, graph: BuildGraph) -> CompileCommands | None:
    """Return the compilation database for *graph*, or ``None`` if it has no modules."""
    chosen = choose_builds(graph, settings)
    if not chosen:
        return None
    headers_dir = paths.intermediate / "CompileCommands"
    files: dict[Path, str] = {}
    entries = []
    for module, toolchain in chosen:
        header = headers_dir / f"{module.node}.h"
        files[header] = "\n".join(module.definitions_header()) + "\n"
        sources = [*module.cpp_sources, *module.c_sources]
        prefixes: dict[str, str] = {}
        for language, used in (("c++", module.cpp_sources), ("c", module.c_sources)):
            if not used:
                continue
            options = toolchain.compile_options(compile_spec(module, settings, language, [header]))
            split = options.index("-c") + 1
            rsp = headers_dir / f"{module.node}.{'cpp' if language == 'c++' else 'c'}.rsp"
            files[rsp] = " ".join(options[split:]) + "\n"
            prefixes[language] = " ".join([f'"{toolchain.compiler}"', *options[:split], f'"@{rsp}"'])
        for source in sources:
            language = "c" if source.suffix in C_EXTENSIONS else "c++"
            output = module.intermediate / module.relative_source(source).with_suffix(toolchain.obj_ext)
            command = prefixes[language].replace("%1", str(source)).replace("%2", str(output))
            entries.append(
                {"directory": str(paths.root), "file": str(source), "output": str(output), "command": command}
            )
    database = paths.root / FILE_NAME
    files[database] = json.dumps(entries, indent=2) + "\n"
    configurations = sorted({str(module.triple.configuration) for module, _ in chosen})
    return CompileCommands(database, len(entries), configurations, files)


def choose_builds(graph: BuildGraph, settings: BuildSettings) -> list[tuple[ModuleBuild, Toolchain]]:
    """Return one build per module and the toolchain that compiles it.

    Prefer the host platform, then the configured configuration, then the shared build environment.
    """
    preferred = settings.compile_commands.configuration
    best: dict[str, tuple[tuple[int, int, int], ModuleBuild, Toolchain]] = {}
    for target in graph.targets:
        for module in target.modules:
            key = (
                int(target.triple.platform is not graph.host.platform),
                int(target.triple.configuration is not preferred),
                int(not module.shared),
            )
            if module.name not in best or key < best[module.name][0]:
                best[module.name] = (key, module, target.toolchain)
    for module in graph.standalone:
        key = (
            int(module.triple.platform is not graph.host.platform),
            int(module.triple.configuration is not preferred),
            0,
        )
        if module.name not in best or key < best[module.name][0]:
            best[module.name] = (key, module, graph.toolchain_for(module.triple))
    return [(module, toolchain) for _, module, toolchain in sorted(best.values(), key=lambda b: b[1].name)]
