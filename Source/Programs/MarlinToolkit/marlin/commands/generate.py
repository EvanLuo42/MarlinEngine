"""Implement ``marlin generate``: write the FASTBuild .bff and the IDE files from the rules files."""

import argparse
import os
import re
import sys
from collections import Counter
from pathlib import Path

from marlin import bff
from marlin.build import inputs
from marlin.build.discovery import DiscoveryError, discover
from marlin.build.graph import Filters, GraphError, build_graph
from marlin.build.model import Arch, Configuration, Platform, parse_enum
from marlin.build.platforms import Host
from marlin.build.settings import SettingsError, find_presets, load_settings
from marlin.build.version import VersionError, load_version
from marlin.build.visualstudio import VisualStudioError
from marlin.fastbuild import run_fbuild
from marlin.paths import EnginePaths, engine_id


def register(subparsers: argparse._SubParsersAction) -> None:
    """Add the ``generate`` subcommand to *subparsers*."""
    parser = subparsers.add_parser(
        "generate",
        help="Generate FASTBuild .bff files and the Visual Studio solution.",
        description="Scan Source/ for *.module.py and *.target.py, resolve every valid "
        "target x platform x arch x configuration, and write Intermediate/FASTBuild/fbuild.bff. "
        "Settings layers, later wins: Build/BuildSettings.toml, Build/BuildSettings.local.toml, "
        "each --preset from Build/Presets, then the other options below.",
    )
    filters = parser.add_argument_group("filters (default: everything valid on this host)")
    filters.add_argument("-t", "--target", action="append", help="only generate this target (repeatable)")
    filters.add_argument("-p", "--platform", action="append", help="only this platform (repeatable)")
    filters.add_argument("-a", "--arch", action="append", help="only this architecture (repeatable)")
    filters.add_argument("-c", "--configuration", action="append", help="only this configuration (repeatable)")
    overrides = parser.add_argument_group("setting overrides")
    overrides.add_argument(
        "--preset",
        action="append",
        default=[],
        metavar="NAME",
        help="apply Build/Presets/NAME.toml on top of the settings files (repeatable, in order)",
    )
    overrides.add_argument(
        "--set",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="override any BuildSettings.toml key, e.g. unity.enabled=false",
    )
    overrides.add_argument("--cache-path", help="FASTBuild cache directory (local, absolute or UNC)")
    overrides.add_argument("--cache", action=argparse.BooleanOptionalAction, help="enable the FASTBuild cache")
    overrides.add_argument("--dist", action=argparse.BooleanOptionalAction, help="enable distributed compilation")
    overrides.add_argument("--unity", action=argparse.BooleanOptionalAction, help="enable unity builds")
    overrides.add_argument("--pch", action=argparse.BooleanOptionalAction, help="enable precompiled headers")
    overrides.add_argument("--solution", action=argparse.BooleanOptionalAction, help="generate the .sln")
    overrides.add_argument(
        "--compile-commands", action=argparse.BooleanOptionalAction, help="write compile_commands.json"
    )
    version = parser.add_argument_group("version (CI stamps these; local builds leave them unset)")
    version.add_argument("--build-id", type=int, help="build number, e.g. the changelist (default: 0)")
    version.add_argument("--branch", help="branch name baked into MARLIN_VERSION_BRANCH")
    parser.add_argument("-v", "--verbose", action="store_true", help="list every excluded combination and why")
    parser.set_defaults(handler=run)


def _overrides(args: argparse.Namespace) -> list[str]:
    """Return the setting overrides of *args* as ``key=value`` strings."""
    result = list(args.set)
    if args.cache_path is not None:
        result.append(f"cache.path={_toml_string(args.cache_path)}")
    for flag, key in (
        ("cache", "cache.enabled"),
        ("dist", "distribution.enabled"),
        ("unity", "unity.enabled"),
        ("pch", "pch.enabled"),
        ("solution", "solution.enabled"),
        ("compile_commands", "compile_commands.enabled"),
    ):
        if (value := getattr(args, flag)) is not None:
            result.append(f"{key}={'true' if value else 'false'}")
    return result


def _toml_string(text: str) -> str:
    """Return *text* as a TOML string literal."""
    return "'" + text + "'" if "'" not in text else '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _enum_set(enum, values: list[str] | None):
    return {parse_enum(enum, v) for v in values} if values else None


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    """Generate for *args*; return the exit code.

    The inputs are recorded before anything is read, so an edit made while generating is caught by
    the next build.
    """
    snapshot = inputs.snapshot(paths)
    invocation = getattr(args, "argv", ["generate"])[1:]
    try:
        presets = find_presets(paths.presets, args.preset)
        settings = load_settings([*paths.build_settings_files, *presets], _overrides(args))
        filters = Filters(
            targets=set(args.target) if args.target else None,
            platforms=_enum_set(Platform, args.platform),
            architectures=_enum_set(Arch, args.arch),
            configurations=_enum_set(Configuration, args.configuration),
        )
        version = load_version(paths.version_file, args.build_id, args.branch)
        discovered = discover(paths)
        host = Host.detect()
        graph = build_graph(paths, discovered, settings, host, version, filters)
        result = bff.generate(paths, settings, graph, invocation, snapshot)
    except (SettingsError, VersionError, VisualStudioError, DiscoveryError, GraphError, ValueError) as error:
        print(f"marlin generate: {error}", file=sys.stderr)
        return 1

    print(f"Marlin {version.full}{f' ({version.branch})' if version.branch else ''}, host {host.platform} {host.arch}")
    print(f"Engine {_register_engine(paths)}")
    if presets:
        print(f"Presets: {', '.join(p.stem for p in presets)}")
    for toolchain in graph.toolchains.values():
        print(f"Toolchain {toolchain.name}: {toolchain.version}")
    print(
        f"Found {len(discovered.modules)} modules ({len(discovered.test_modules)} test), "
        f"{len(discovered.targets)} targets; "
        f"generated {len(graph.targets)} configurations, excluded {len(graph.excluded)}."
    )
    _report_exclusions(graph, args.verbose)
    print(
        f"Unity: {'on' if settings.unity.enabled else 'off'}, PCH: {'on' if settings.pch.enabled else 'off'}, "
        f"cache: {paths.resolve(settings.cache.path) if settings.cache.enabled else 'off'} "
        f"({settings.cache.mode if settings.cache.enabled else '-'}), "
        f"distribution: {'on' if settings.distribution.enabled else 'off'}"
    )
    print(f"Wrote {result.bff}")
    if result.compile_commands:
        commands = result.compile_commands
        print(f"Wrote {commands.path} ({commands.sources} sources, {', '.join(commands.configurations)})")
    if result.solution:
        sys.stdout.flush()
        code = run_fbuild(paths, [bff.SOLUTION_ALIAS], ["-quiet"])
        if code != 0:
            return code
        vs = result.solution.visual_studio
        where = vs.version if vs.installed else "not installed on this machine"
        print(f"Wrote {result.solution.path} (Visual Studio {vs.year}, {where})")
        if vs.installed and result.solution.intellisense != "clang":
            print(
                "  IntelliSense uses MSVC rules; install the Visual Studio component "
                "'C++ Clang tools for Windows' and re-run generate for clang mode"
            )
    return 0


def _register_engine(paths: EnginePaths) -> str:
    """Record this source engine in the user's engine registry, so projects bound to its Id find it; return the Id.

    Windows keeps it in HKCU/Software/Marlin/Engines, other hosts in Marlin/Engines.mtext under the user's
    config directory (Documentation/Architecture/02 §3).
    """
    identifier = engine_id(paths.root)
    root = str(paths.root.resolve()).replace("\\", "/")
    if sys.platform == "win32":
        import winreg

        try:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Marlin\Engines") as key:
                winreg.SetValueEx(key, identifier, 0, winreg.REG_SZ, root)
        except OSError as error:
            # A locked-down machine or a key whose ACL denies this user: the build does not need the entry, only
            # MarlinLauncher does, so say where it is missing and continue.
            print(
                f"marlin generate: warning: cannot register {identifier} in HKCU\\Software\\Marlin\\Engines: {error}",
                file=sys.stderr,
            )
            print(
                f"  {paths.root} still generates and builds; projects bound to {identifier} need -Project= or a "
                "fixed registry permission.",
                file=sys.stderr,
            )
        return identifier
    config = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    if sys.platform == "darwin":
        config = Path.home() / "Library" / "Application Support"
    registry = config / "Marlin" / "Engines.mtext"
    entries = (
        dict(re.findall(r'^\s*"([^"]+)"\s*=\s*"([^"]*)"', registry.read_text(encoding="utf-8"), re.MULTILINE))
        if registry.is_file()
        else {}
    )
    entries[identifier] = root
    lines = ["Engines", "{", *(f'    "{k}" = "{v}"' for k, v in sorted(entries.items())), "}", ""]
    registry.parent.mkdir(parents=True, exist_ok=True)
    registry.write_text("\n".join(lines), encoding="utf-8")
    return identifier


def _report_exclusions(graph, verbose: bool) -> None:
    """Print the excluded combinations; without *verbose*, list only the unexpected ones."""
    if not graph.excluded:
        return
    if verbose:
        for exclusion in graph.excluded:
            print(f"  skip {exclusion.target}-{exclusion.triple}: {exclusion.reason}")
        return
    reasons = Counter(e.reason for e in graph.excluded if not e.structural)
    for reason, count in reasons.most_common():
        print(f"  {count:4d} excluded: {reason}")
    structural = sum(e.structural for e in graph.excluded)
    if structural:
        print(f"  {structural:4d} excluded as invalid on this host or for the target type (-v to list)")
