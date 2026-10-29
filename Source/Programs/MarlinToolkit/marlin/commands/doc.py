"""Implement ``marlin doc``: the manual and the API reference (Documentation/Architecture/16).

The toolkit decides what MarlinDoc reads: every module's public headers and the options they compile with, the
libraries under ``Language/`` and the books under ``Documentation/``. It writes them into a job file, builds MarlinDoc
and the header tool runs whose ``.gen.h`` the headers include, and runs MarlinDoc on the job.

MarlinDoc writes the JSON data (§4); the static site made from it (§5) is not implemented yet.
"""

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

from marlin.bff import write_if_changed
from marlin.bff.compdb import choose_builds
from marlin.build.discovery import DiscoveryError, ModuleKind, discover
from marlin.build.graph import BuildGraph, Filters, GraphError, ModuleBuild, build_graph, compile_spec
from marlin.build.model import Configuration
from marlin.build.platforms import Host
from marlin.build.settings import BuildSettings, SettingsError, load_settings
from marlin.build.toolchain import Toolchain, ToolchainUnavailable, find_llvm
from marlin.build.version import VersionError, load_version
from marlin.commands.build import regenerate_if_stale
from marlin.fastbuild import load_manifest, run_fbuild
from marlin.paths import EnginePaths

JOB_FORMAT_VERSION = 1
DOC_TARGET = "MarlinDoc"
DOC_CONFIGURATION = Configuration.Development
HEADER_SUFFIXES = (".h", ".hpp", ".inl")
# The dialect folders under Language/ (MarlinLanguage §1.5) and the language each holds.
LANGUAGE_FOLDERS = {"Common": "mcommon", "Shaders": "mshader", "Scripts": "mscript"}
LANGUAGE_EXTENSIONS = {"mcommon": ".mcommon", "mshader": ".mshader", "mscript": ".mscript"}


def register(subparsers: argparse._SubParsersAction) -> None:
    """Add the ``doc`` subcommand to *subparsers*."""
    parser = subparsers.add_parser(
        "doc",
        help="Generate the manual and the API reference.",
        description="Extract the `///` docs of the engine's C++ modules and Language/ libraries, read the books "
        "under Documentation/, resolve their links and write them as JSON data (Documentation/Architecture/16). "
        "The static site made from the data is not implemented yet, so --data-only is what runs either way.",
    )
    parser.add_argument("modules", nargs="*", help="only document these modules (default: all)")
    parser.add_argument("--data-only", action="store_true", help="write the JSON data but no site")
    parser.add_argument("--no-manual", action="store_true", help="leave out the books under Documentation/")
    parser.add_argument("--no-reference", action="store_true", help="leave out the API reference")
    parser.add_argument("--out", help="output directory (default: Intermediate/Docs/<Platform>)")
    parser.add_argument("--deny-warnings", action="store_true", help="fail if anything was reported")
    parser.add_argument("--missing-docs", action="store_true", help="list each public item without docs")
    parser.add_argument("--no-build", action="store_true", help="run MarlinDoc without building it first")
    parser.set_defaults(handler=run)


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    """Generate the documentation for *args*; return the exit code."""
    manifest = load_manifest(paths)
    if not manifest:
        print("marlin doc: no build files; run `marlin generate` first", file=sys.stderr)
        return 1
    if not args.no_build and (code := regenerate_if_stale(paths, manifest)) != 0:
        return code
    host = Host.detect()
    try:
        settings = load_settings(paths.build_settings_files, [])
        filters = Filters(platforms={host.platform}, architectures={host.arch}, configurations={DOC_CONFIGURATION})
        graph = build_graph(
            paths, discover(paths), settings, host, load_version(paths.version_file, None, None), filters
        )
        libclang = _find_libclang(find_llvm(settings, paths.root))
    except (SettingsError, VersionError, DiscoveryError, GraphError, ToolchainUnavailable, ValueError) as error:
        print(f"marlin doc: {error}", file=sys.stderr)
        return 1
    target = next((t for t in graph.targets if t.name == DOC_TARGET), None)
    if target is None:
        print(f"marlin doc: no {DOC_TARGET} target for {host.platform} {host.arch}", file=sys.stderr)
        return 1
    if libclang is None and not args.no_reference:
        print(
            "marlin doc: libclang not found in the LLVM install; install LLVM's libclang (on Linux the libclang-<N> "
            "package) or set toolchain.llvm_root",
            file=sys.stderr,
        )
        return 1

    out = paths.resolve(args.out) if args.out else paths.intermediate / "Docs" / str(host.platform)
    jobs = out / "Job"
    wanted = {m.lower() for m in args.modules} if args.modules else None
    modules = [] if args.no_reference else _modules(paths, settings, graph, jobs, wanted)
    languages = [] if args.no_reference else _languages(paths, wanted)
    if wanted is not None and not modules and not languages:
        print(f"marlin doc: no module named {', '.join(args.modules)}", file=sys.stderr)
        return 1
    job: dict[str, Any] = {
        "format_version": JOB_FORMAT_VERSION,
        "root": str(paths.root),
        "platform": str(host.platform),
        "output": str(out),
        "data_only": args.data_only,
        "libclang": str(libclang) if libclang else "",
        "missing_docs": args.missing_docs,
        "modules": [entry for entry, _ in modules],
        "languages": languages,
        "books": [] if args.no_manual else _books(paths),
    }
    job_file = jobs / "Job.json"
    write_if_changed(job_file, json.dumps(job, indent=2) + "\n")

    executable = target.binaries / target.toolchain.exe_name(target.output_name)
    if not args.no_build:
        nodes = [target.alias, *sorted({node for _, node in modules if node})]
        if (code := run_fbuild(paths, nodes, ["-quiet", *manifest.get("fbuild_args", [])])) != 0:
            return code
    print(f"marlin doc: {len(modules)} modules, {len(languages)} libraries, {len(job['books'])} books")
    sys.stdout.flush()
    try:
        code = subprocess.run([str(executable), f"-Job={job_file}"], check=False, cwd=paths.root).returncode
    except OSError as error:
        print(f"marlin doc: cannot run {executable}: {error}", file=sys.stderr)
        return 1
    if code == 2 and not args.deny_warnings:
        code = 0
    if code == 0:
        print(f"marlin doc: wrote {out / 'Data'}")
        if not args.data_only:
            print("marlin doc: the static site is not implemented yet (Documentation/Architecture/16 §5)")
    return code


def _find_libclang(llvm_root: Path) -> Path | None:
    """Return the libclang of the LLVM install at *llvm_root*, or ``None``."""
    for candidate in (
        llvm_root / "bin" / "libclang.dll",
        llvm_root / "lib" / "libclang.so",
        llvm_root / "lib" / "libclang.dylib",
    ):
        if candidate.is_file():
            return candidate
    matches = sorted((llvm_root / "lib").glob("libclang.so.*"))
    return matches[-1] if matches else None


def _modules(
    paths: EnginePaths, settings: BuildSettings, graph: BuildGraph, jobs: Path, wanted: set[str] | None
) -> list[tuple[dict[str, Any], str | None]]:
    """Return the job entry of each module with public headers, and the header tool run its headers need."""
    result = []
    for module, toolchain in choose_builds(graph, settings):
        kind = module.descriptor.kind
        if kind in (ModuleKind.ThirdParty, ModuleKind.Test) or module.descriptor.tested_module is not None:
            continue
        if wanted is not None and module.name.lower() not in wanted:
            continue
        headers = _public_headers(module)
        if not headers:
            continue
        definitions = jobs / f"{module.name}.h"
        write_if_changed(definitions, "\n".join(module.definitions_header()) + "\n")
        docs = module.descriptor.dir / "Docs.md"
        entry = {
            "name": module.name,
            "kind": str(kind),
            "directory": _relative(paths, module.descriptor.dir),
            "docs": str(docs) if docs.is_file() else "",
            "headers": [str(h) for h in headers],
            "arguments": _arguments(module, toolchain, settings, definitions),
        }
        node = (
            f"HeaderTool-{module.triple.platform}-{module.triple.arch}-{module.name}"
            if module.generated is not None
            else None
        )
        result.append((entry, node))
    return result


def _public_headers(module: ModuleBuild) -> list[Path]:
    """Return the headers under the ``Public/`` folders of *module*'s roots; a higher root's copy of a path wins."""
    found: dict[Path, Path] = {}
    for root in module.roots:
        public = root / "Public"
        if not public.is_dir():
            continue
        for header in sorted(public.rglob("*")):
            if header.suffix not in HEADER_SUFFIXES or header.name.endswith(".gen.h"):
                continue
            if any(header.is_relative_to(nested) for nested in module.nested_module_dirs):
                continue
            found.setdefault(header.relative_to(public), header)
    return [found[relative] for relative in sorted(found)]


def _arguments(module: ModuleBuild, toolchain: Toolchain, settings: BuildSettings, definitions: Path) -> str:
    """Return the compiler options of *module*'s C++ sources as one command line, without input and output."""
    options = toolchain.compile_options(compile_spec(module, settings, "c++", [definitions]))
    return " ".join(options[options.index("-c") + 1 :])


def _languages(paths: EnginePaths, wanted: set[str] | None) -> list[dict[str, Any]]:
    """Return the libraries under ``Language/``: each folder of a dialect folder but Tests, named like
    ``Scripts.Core``."""
    result = []
    language_root = paths.root / "Language"
    for folder, language in LANGUAGE_FOLDERS.items():
        dialect = language_root / folder
        if not dialect.is_dir():
            continue
        for library in sorted(p for p in dialect.iterdir() if p.is_dir() and p.name != "Tests"):
            name = f"{folder}.{library.name}"
            if wanted is not None and name.lower() not in wanted:
                continue
            files = sorted(library.rglob(f"*{LANGUAGE_EXTENSIONS[language]}"))
            if files:
                result.append(
                    {
                        "name": name,
                        "language": language,
                        "directory": _relative(paths, library),
                        "files": [str(f) for f in files],
                    }
                )
    return result


def _books(paths: EnginePaths) -> list[str]:
    """Return the folders of ``Documentation/`` that hold a ``Summary.md``: one book each."""
    documentation = paths.root / "Documentation"
    if not documentation.is_dir():
        return []
    return [str(d) for d in sorted(documentation.iterdir()) if (d / "Summary.md").is_file()]


def _relative(paths: EnginePaths, path: Path) -> str:
    """Return *path* relative to the engine root, with forward slashes."""
    return path.relative_to(paths.root).as_posix()
