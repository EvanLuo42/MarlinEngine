"""Resolve discovered rules into build plans for every target, platform, arch and configuration.

Modules are compiled once per build environment. Every target of the same platform, arch and
configuration shares one environment, whatever its type, unless it sets its own global definitions;
module code therefore cannot tell the editor from the game, and the link mode is set per
configuration. Linking is always per target.

Include visibility follows the module layout: a module sees its own ``Public/``, ``Internal/`` and
``Private/``, and its dependents see ``Public/`` and ``Internal/``. ``Internal/`` holds engine-wide
headers that are not part of the API offered to projects, and will be hidden from them once they
exist.

A module is made of its directory and the overlays of the layers above it in the build's layer stack
(see :mod:`marlin.build.discovery`), highest first. Each of them adds its ``Public/``, ``Internal/``
and ``Private/`` include paths in that order, so a header found in a more specific layer hides the
same header below it; headers should therefore include each other by their full path from the
include folder, since a quoted include is looked up next to the including file first. A source file
at the same path relative to its root replaces the ones below it. A build whose platform lacks one of
its layers is left out, and so is a module defined in a layer the build does not use.

A module that no target links is still resolved in the shared environment of each combination some
target is built for, so that it has a project and compiles on its own.

A test module ``<Module>Tests`` depends on the module it tests, everything that module depends on, and the
``Testing`` framework module, and also sees the tested module's ``Private/`` headers. In modular
builds the tests still link against the tested module's DLL, so they can only call what it exports.
Nothing may depend on a test module; targets with ``test_modules`` set pull them in.

Every module that sees Core gets its reflection code from ``MarlinHeaderTool`` (Documentation/Architecture/04),
written to ``Intermediate/Generated/<Platform>/<Arch>/<Module>``: the ``Include/`` there is on the include path of
the module and its dependents, and the ``Source/`` is compiled into the module. The header tool depends on no engine
module, so it is built first, for the host in Development, whatever the filters.
"""

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

from marlin.build.discovery import (
    Discovered,
    ModuleDescriptor,
    ModuleKind,
    TargetDescriptor,
)
from marlin.build.model import (
    Arch,
    Configuration,
    LinkMode,
    Platform,
    TargetType,
    Triple,
)
from marlin.build.platforms import Host, invalid_reason, layer_stack, layer_table
from marlin.build.settings import BuildSettings
from marlin.build.toolchain import CompileSpec, Toolchain, ToolchainUnavailable, make_toolchain
from marlin.build.version import EngineVersion
from marlin.paths import EnginePaths
from marlin.rules import (
    BuildContext,
    ModuleRules,
    SharedLibrary,
    TargetRules,
    ThirdPartyRules,
    Unsupported,
)

CPP_EXTENSIONS = (".cpp", ".cc", ".cxx")
C_EXTENSIONS = (".c",)
OBJC_EXTENSIONS = (".m", ".mm")
RC_EXTENSIONS = (".rc",)
SHARED_ENV = "Shared"
OWN_INCLUDE_DIRS = ("Public", "Internal", "Private")
SHARED_INCLUDE_DIRS = ("Public", "Internal")
TEST_FRAMEWORK_MODULE = "Testing"
CODEGEN_MODULE = "Core"
HEADER_TOOL_TARGET = "MarlinHeaderTool"
HEADER_TOOL_CONFIGURATION = Configuration.Development


class GraphError(Exception):
    """Raised for rules that cannot form a valid build graph."""


class _Invalid(Unsupported):
    """Exclude a combination that the platform matrix or the target's restrictions rule out."""


@dataclass
class ThirdPartyBuild:
    """A third-party library resolved for one build combination.

    *shared_libraries* holds ``(runtime, link)`` pairs; *link* is ``None`` when the runtime file is
    also the linker input.
    """

    name: str
    include_paths: list[Path]
    definitions: list[str]
    system_libraries: list[str]
    frameworks: list[str]
    link_options: list[str]
    static_libraries: list[Path]
    shared_libraries: list[tuple[Path, Path | None]]


@dataclass
class ModuleBuild:
    """A module compiled in one build environment.

    *roots* are the module's overlays on this build's layer stack and its own directory, highest
    first. *overridden_sources* are the sources below them that a higher root replaces.
    *nested_module_dirs* are module directories below the roots, whose sources belong to them.
    *visible_modules* are the engine modules whose headers this module sees and, when modular,
    links against. *generated* is where the header tool writes the module's code, or ``None`` for a
    module that does not see Core; *generated_dependencies* are the modules with generated code whose
    headers this module includes, itself first if it has any.
    """

    name: str
    descriptor: ModuleDescriptor
    rules: ModuleRules
    triple: Triple
    env_key: str
    intermediate: Path
    roots: list[Path]
    cpp_sources: list[Path]
    c_sources: list[Path]
    rc_sources: list[Path]
    overridden_sources: list[Path]
    unity: bool
    unity_exclude: set[Path]
    nested_module_dirs: list[Path]
    pch: Path | None
    include_paths: list[Path]
    system_include_paths: list[Path]
    definitions: list[str]
    warnings_as_errors: bool
    visible_modules: list[str]
    visible_third_party: list[str]
    generated: Path | None = None
    generated_dependencies: list[str] = field(default_factory=list)

    @property
    def node(self) -> str:
        return f"{self.name}-{self.env_key}"

    @property
    def shared(self) -> bool:
        return self.env_key.endswith(f"-{SHARED_ENV}")

    def relative_source(self, source: Path) -> Path:
        """Return *source* relative to the root it belongs to."""
        return next(source.relative_to(root) for root in self.roots if source.is_relative_to(root))

    def definitions_header(self) -> list[str]:
        """Return the lines of the header that defines *definitions* for every source."""
        lines = ["// Generated by `marlin generate`. Do not edit.", "#pragma once"]
        for definition in self.definitions:
            name, sep, value = definition.partition("=")
            lines.append(f"#define {name} {value if sep else '1'}".rstrip())
        return lines


def compile_spec(
    module: ModuleBuild, settings: BuildSettings, language: Literal["c", "c++"], force_includes: list[Path]
) -> CompileSpec:
    """Return how *module* compiles its *language* sources."""
    configuration = settings.configurations[module.triple.configuration]
    return CompileSpec(
        language=language,
        standard=settings.c_standard if language == "c" else settings.cpp_standard,
        optimize=configuration.optimize,
        lto=configuration.lto,
        debug_info=configuration.debug_info,
        warnings_as_errors=module.warnings_as_errors,
        sanitizers=configuration.sanitizers,
        include_paths=module.include_paths,
        system_include_paths=module.system_include_paths,
        force_includes=force_includes,
        exceptions=module.rules.allow_exceptions,
        rtti=module.rules.allow_rtti,
        extra=list(module.rules.compile_options),
    )


@dataclass
class TargetBuild:
    """A target resolved for one build combination; *modules* lists dependencies first."""

    name: str
    descriptor: TargetDescriptor
    rules: TargetRules
    triple: Triple
    ctx: BuildContext
    toolchain: Toolchain
    env_key: str
    modules: list[ModuleBuild]
    third_party: dict[str, ThirdPartyBuild]
    binaries: Path
    intermediate: Path

    @property
    def alias(self) -> str:
        return f"{self.name}-{self.triple}"

    @property
    def target_type(self) -> TargetType:
        return self.rules.type

    @property
    def output_name(self) -> str:
        return self.rules.output_name or self.name

    @property
    def launch_module(self) -> ModuleBuild:
        return next(m for m in self.modules if m.name == self.rules.launch_module)

    @property
    def test_modules(self) -> list[ModuleBuild]:
        return [m for m in self.modules if m.descriptor.tested_module is not None]


@dataclass
class Exclusion:
    """A build combination left out of the graph.

    Structural exclusions, such as Linux on a Windows host, are expected. The others, such as a
    missing SDK or a rules file opting out, are worth reporting.
    """

    target: str
    triple: Triple
    reason: str
    structural: bool


@dataclass
class BuildGraph:
    """The target builds of one generate, with the exclusions and toolchains involved.

    *standalone* holds the builds of modules that no target links in their combination, so that
    they can be compiled and browsed on their own. *header_tool* is the build of MarlinHeaderTool that
    generates the reflection code; it is also one of *targets*.
    """

    host: Host
    version: EngineVersion
    targets: list[TargetBuild] = field(default_factory=list)
    excluded: list[Exclusion] = field(default_factory=list)
    toolchains: dict[str, Toolchain] = field(default_factory=dict)
    standalone: list[ModuleBuild] = field(default_factory=list)
    header_tool: TargetBuild | None = None

    @property
    def module_builds(self) -> list[ModuleBuild]:
        """Return each module build once, however many targets share it."""
        seen: dict[str, ModuleBuild] = {}
        for target in self.targets:
            for module in target.modules:
                seen.setdefault(module.node, module)
        for module in self.standalone:
            seen.setdefault(module.node, module)
        return list(seen.values())

    def toolchain_for(self, triple: Triple) -> Toolchain:
        """Return the toolchain of a combination that some target is built for."""
        return next(t for t in self.toolchains.values() if t.platform is triple.platform and t.arch is triple.arch)


@dataclass
class Filters:
    """Restrict the generated combinations; ``None`` allows everything."""

    targets: set[str] | None = None
    platforms: set[Platform] | None = None
    architectures: set[Arch] | None = None
    configurations: set[Configuration] | None = None


def build_graph(
    paths: EnginePaths,
    discovered: Discovered,
    settings: BuildSettings,
    host: Host,
    version: EngineVersion,
    filters: Filters,
) -> BuildGraph:
    """Resolve every discovered target for every combination that passes *filters*.

    Combinations that cannot be built are recorded in ``BuildGraph.excluded``. Raise
    :exc:`GraphError` for an unknown target name or invalid rules.
    """
    graph = BuildGraph(host, version)
    resolver = _Resolver(paths, discovered, settings, graph)
    unknown = (filters.targets or set()) - set(discovered.targets)
    if unknown:
        raise GraphError(f"unknown target(s): {', '.join(sorted(unknown))}")
    for target in discovered.targets.values():
        if filters.targets is not None and target.name not in filters.targets:
            continue
        for platform in Platform:
            if filters.platforms is not None and platform not in filters.platforms:
                continue
            for arch in Arch:
                if filters.architectures is not None and arch not in filters.architectures:
                    continue
                for configuration in Configuration:
                    if filters.configurations is not None and configuration not in filters.configurations:
                        continue
                    triple = Triple(platform, arch, configuration)
                    try:
                        graph.targets.append(resolver.resolve_target(target, triple))
                    except Unsupported as reason:
                        structural = isinstance(reason, _Invalid)
                        graph.excluded.append(Exclusion(target.name, triple, str(reason), structural))
    if HEADER_TOOL_TARGET in discovered.targets:
        resolver.resolve_header_tool()
    if filters.targets is None:
        resolver.resolve_standalone()
    return graph


class _Resolver:
    """Evaluate rules and resolve builds, caching what build combinations share."""

    def __init__(self, paths: EnginePaths, discovered: Discovered, settings: BuildSettings, graph: BuildGraph) -> None:
        self.paths = paths
        self.discovered = discovered
        self.settings = settings
        self.graph = graph
        self._toolchain_errors: dict[tuple[Platform, Arch], str] = {}
        self._rules: dict[tuple[str, BuildContext], ModuleRules | ThirdPartyRules] = {}
        self._modules: dict[str, ModuleBuild] = {}
        self._third_party: dict[tuple[str, BuildContext], ThirdPartyBuild] = {}
        self._sources: dict[tuple[str, tuple[Path, ...], Platform], tuple[list[Path], list[Path]]] = {}
        self._existing_dirs: dict[Path, bool] = {}

    def toolchain(self, platform: Platform, arch: Arch) -> Toolchain:
        """Return the toolchain for *platform* and *arch*, or raise :exc:`Unsupported`."""
        key = (platform, arch)
        if key in self._toolchain_errors:
            raise Unsupported(self._toolchain_errors[key])
        for toolchain in self.graph.toolchains.values():
            if toolchain.platform is platform and toolchain.arch is arch:
                return toolchain
        try:
            toolchain = make_toolchain(platform, arch, self.settings, self.paths.root)
        except ToolchainUnavailable as error:
            self._toolchain_errors[key] = str(error)
            raise Unsupported(str(error)) from error
        self.graph.toolchains[toolchain.name] = toolchain
        return toolchain

    def resolve_target(self, target: TargetDescriptor, triple: Triple) -> TargetBuild:
        rules_class = target.rules_class
        target_type = rules_class.type
        if reason := invalid_reason(self.graph.host, triple.platform, triple.arch, triple.configuration, target_type):
            raise _Invalid(reason)
        for attribute, value in (
            ("platforms", triple.platform),
            ("architectures", triple.arch),
            ("configurations", triple.configuration),
        ):
            allowed = getattr(rules_class, attribute)
            if allowed is not None and value not in allowed:
                raise _Invalid(f"{target.name} restricts {attribute} to {', '.join(sorted(map(str, allowed)))}")
        missing = [layer for layer in layer_stack(triple.platform, triple.arch) if layer not in self.discovered.layers]
        if missing:
            raise _Invalid(f"platform layer(s) {', '.join(missing)} not found under {self.paths.platforms}")

        ctx = self._context(triple)
        rules = rules_class(target.name, target.rules_file.parent)
        rules.setup(ctx)
        if not rules.launch_module:
            raise GraphError(f"{target.rules_file}: launch_module is not set")
        toolchain = self.toolchain(triple.platform, triple.arch)

        env_name = target.name if rules.definitions else SHARED_ENV
        env_key = f"{triple}-{env_name}"
        roots = [rules.launch_module, *rules.extra_modules]
        if rules.test_modules:
            roots += self._test_modules(ctx, target, target_type)
        order = self._closure(roots, ctx, target.name, target_type)
        modules = [
            self._module_build(name, ctx, env_key, env_name, triple, rules.definitions, toolchain)
            for name in order
            if not self.discovered.modules[name].is_third_party
        ]
        third_party = {
            name: self._third_party_build(name, ctx) for name in order if self.discovered.modules[name].is_third_party
        }
        return TargetBuild(
            name=target.name,
            descriptor=target,
            rules=rules,
            triple=triple,
            ctx=ctx,
            toolchain=toolchain,
            env_key=env_key,
            modules=modules,
            third_party=third_party,
            binaries=self.paths.binaries / str(triple.platform) / str(triple.arch) / str(triple.configuration),
            intermediate=self.paths.intermediate_build
            / str(triple.platform)
            / str(triple.arch)
            / str(triple.configuration)
            / target.name,
        )

    def resolve_header_tool(self) -> None:
        """Resolve MarlinHeaderTool for the host in Development, unless the filters already did."""
        triple = Triple(self.graph.host.platform, self.graph.host.arch, HEADER_TOOL_CONFIGURATION)
        build = next((t for t in self.graph.targets if t.name == HEADER_TOOL_TARGET and t.triple == triple), None)
        if build is None:
            try:
                build = self.resolve_target(self.discovered.targets[HEADER_TOOL_TARGET], triple)
            except Unsupported as reason:
                raise GraphError(f"{HEADER_TOOL_TARGET} cannot be built for the host: {reason}") from reason
            self.graph.targets.append(build)
        if CODEGEN_MODULE in (m.name for m in build.modules):
            raise GraphError(f"{HEADER_TOOL_TARGET} must not depend on {CODEGEN_MODULE}, whose code it generates")
        self.graph.header_tool = build

    def resolve_standalone(self) -> None:
        """Resolve, in each combination some target is built for, the modules no target links there.

        Third-party and test modules are left out, as are modules the combination does not support.
        """
        linked = {m.node for m in self.graph.module_builds}
        for triple in sorted({t.triple for t in self.graph.targets}):
            ctx = self._context(triple)
            toolchain = self.graph.toolchain_for(triple)
            env_key = f"{triple}-{SHARED_ENV}"
            for name, descriptor in self.discovered.modules.items():
                skipped = descriptor.is_third_party or descriptor.kind is ModuleKind.Test
                if skipped or f"{name}-{env_key}" in linked:
                    continue
                if descriptor.kind is ModuleKind.Editor and ctx.is_shipping:
                    continue
                try:
                    order = self._closure([name], ctx, name, TargetType.Editor)
                except Unsupported:
                    continue
                for module in order:
                    if self.discovered.modules[module].is_third_party:
                        continue
                    build = self._module_build(module, ctx, env_key, SHARED_ENV, triple, [], toolchain)
                    if build.node not in linked:
                        linked.add(build.node)
                        self.graph.standalone.append(build)

    def _context(self, triple: Triple) -> BuildContext:
        link_mode = self.settings.configurations[triple.configuration].link
        if triple.platform.is_console:
            link_mode = LinkMode.Monolithic
        return BuildContext(triple.platform, triple.arch, triple.configuration, link_mode)

    def rules_for(self, name: str, ctx: BuildContext) -> ModuleRules | ThirdPartyRules:
        key = (name, ctx)
        if key not in self._rules:
            descriptor = self.discovered.modules[name]
            rules = descriptor.rules_class(name, descriptor.dir)
            rules.setup(ctx)
            self._rules[key] = rules
        return self._rules[key]

    def _test_modules(self, ctx: BuildContext, target: TargetDescriptor, target_type: TargetType) -> list[str]:
        """Return the test modules that a target with ``test_modules`` holds for *ctx*."""
        result: list[str] = []
        for test in self.discovered.test_modules:
            assert test.tested_module is not None
            tested = self.discovered.modules[test.tested_module]
            if tested.kind is ModuleKind.Editor and target_type is not TargetType.Editor:
                continue
            try:
                self._closure([test.name], ctx, target.name, target_type)
            except Unsupported:
                continue
            result.append(test.name)
        return result

    def _deps(self, name: str, ctx: BuildContext, public_only: bool = False) -> list[str]:
        rules = self.rules_for(name, ctx)
        deps = list(rules.public_dependencies)
        if not public_only and isinstance(rules, ModuleRules):
            deps += rules.private_dependencies
        tested = self.discovered.modules[name].tested_module
        if tested is not None and not public_only:
            deps += [tested, *self._deps(tested, ctx), TEST_FRAMEWORK_MODULE]
        return _unique(deps)

    def _closure(self, roots: list[str], ctx: BuildContext, owner: str, target_type: TargetType) -> list[str]:
        """Return all modules reachable from *roots*, dependencies first.

        Raise :exc:`GraphError` on a cycle or a dependency that the module kinds forbid.
        """
        order: list[str] = []
        state: dict[str, str] = {}

        def visit(name: str, stack: list[str]) -> None:
            if name not in self.discovered.modules:
                via = f" (required by {stack[-1]})" if stack else ""
                raise GraphError(f"{owner}: unknown module '{name}'{via}")
            if state.get(name) == "done":
                return
            if state.get(name) == "visiting":
                cycle = " -> ".join([*stack[stack.index(name) :], name])
                raise GraphError(f"dependency cycle: {cycle}")
            state[name] = "visiting"
            self._check_kind(name, stack[-1] if stack else None, ctx, owner, target_type)
            for dep in self._deps(name, ctx):
                visit(dep, [*stack, name])
            state[name] = "done"
            order.append(name)

        for root in roots:
            visit(root, [])
        return order

    def _check_kind(
        self, name: str, parent: str | None, ctx: BuildContext, owner: str, target_type: TargetType
    ) -> None:
        descriptor = self.discovered.modules[name]
        kind = descriptor.kind
        if parent is not None:
            parent_kind = self.discovered.modules[parent].kind
            if kind is ModuleKind.Test:
                raise GraphError(f"'{parent}' cannot depend on test module '{name}'")
            if parent_kind is ModuleKind.Runtime and kind in (ModuleKind.Editor, ModuleKind.Developer):
                raise GraphError(f"runtime module '{parent}' cannot depend on {kind} module '{name}'")
            if parent_kind is ModuleKind.ThirdParty and kind is not ModuleKind.ThirdParty:
                raise GraphError(f"third-party module '{parent}' cannot depend on engine module '{name}'")
        if kind is ModuleKind.Editor and target_type is not TargetType.Editor:
            raise GraphError(f"{owner} ({target_type}) cannot include editor module '{name}'")
        if kind is ModuleKind.Developer and ctx.is_shipping:
            raise Unsupported(f"developer module '{name}' is not allowed in Shipping")
        if descriptor.layer is not None and descriptor.layer not in layer_stack(ctx.platform, ctx.arch):
            raise Unsupported(f"module '{name}' is in the {descriptor.layer} layer, which {ctx.platform} does not use")

    def _visible(self, name: str, ctx: BuildContext) -> list[str]:
        """Return the modules *name* sees: its dependencies and their public closure."""
        seen: list[str] = []

        def add_public(dep: str) -> None:
            if dep in seen:
                return
            seen.append(dep)
            for public in self._deps(dep, ctx, public_only=True):
                add_public(public)

        for dep in self._deps(name, ctx):
            add_public(dep)
        return seen

    def _module_build(
        self,
        name: str,
        ctx: BuildContext,
        env_key: str,
        env_name: str,
        triple: Triple,
        target_definitions: list[str],
        toolchain: Toolchain,
    ) -> ModuleBuild:
        """Return the build of module *name* in the environment *env_key*.

        A module without ``Public/``, ``Internal/`` and ``Private/`` gets its own directory as a
        private include path.
        """
        node = f"{name}-{env_key}"
        if node in self._modules:
            return self._modules[node]
        descriptor = self.discovered.modules[name]
        rules = self.rules_for(name, ctx)
        assert isinstance(rules, ModuleRules)
        _check_frameworks(rules, descriptor.rules_file, ctx)
        if (rules.allow_exceptions or rules.allow_rtti) and descriptor.kind is not ModuleKind.Program:
            raise GraphError(f"{descriptor.rules_file}: only program modules may set allow_exceptions or allow_rtti")
        visible = self._visible(name, ctx)
        visible_modules = [m for m in visible if not self.discovered.modules[m].is_third_party]
        visible_third_party = [m for m in visible if self.discovered.modules[m].is_third_party]

        _check_public_include_paths(descriptor, rules)
        roots = self._roots(descriptor, ctx)
        include_paths = self._own_include_paths(roots, rules)
        if descriptor.tested_module is not None:
            tested = self.discovered.modules[descriptor.tested_module]
            tested_rules = self.rules_for(tested.name, ctx)
            assert isinstance(tested_rules, ModuleRules)
            include_paths += self._own_include_paths(self._roots(tested, ctx), tested_rules)
        definitions = [*rules.private_definitions, *rules.public_definitions]
        for dep in visible_modules:
            dep_roots = self._roots(self.discovered.modules[dep], ctx)
            dep_rules = self.rules_for(dep, ctx)
            include_paths += [
                root / sub for root in dep_roots for sub in SHARED_INCLUDE_DIRS if self._is_dir(root / sub)
            ]
            include_paths += self._layered(dep_roots, dep_rules.public_include_paths)
            definitions += dep_rules.public_definitions
        generated = None
        generated_dependencies: list[str] = []
        if HEADER_TOOL_TARGET in self.discovered.targets:
            if self._has_generated_code(name, ctx):
                generated = self._generated_dir(name, triple)
                generated_dependencies.append(name)
            generated_dependencies += [m for m in visible_modules if self._has_generated_code(m, ctx)]
            include_paths += [self._generated_dir(m, triple) / "Include" for m in generated_dependencies]
        include_paths.append(self.paths.generated_headers)
        system_include_paths: list[Path] = []
        for dep in visible_third_party:
            third_party = self._third_party_build(dep, ctx)
            system_include_paths += third_party.include_paths
            definitions += third_party.definitions

        api = _api_definitions(name, visible_modules, ctx)
        global_definitions = self._global_definitions(ctx, triple, name)
        sources, overridden = self._module_sources(name, roots, triple.platform)
        cpp_sources = [s for s in sources if s.suffix in CPP_EXTENSIONS + OBJC_EXTENSIONS]
        c_sources = [s for s in sources if s.suffix in C_EXTENSIONS]
        rc_sources = [s for s in sources if s.suffix in RC_EXTENSIONS]
        unity = self.settings.unity.enabled if rules.unity is None else rules.unity
        unity = unity and len(cpp_sources) >= self.settings.unity.min_files
        pch = _first_existing(roots, rules.pch) if rules.pch and self.settings.pch.enabled else None
        if pch is not None and not pch.is_file():
            raise GraphError(f"{descriptor.rules_file}: pch '{rules.pch}' does not exist")

        build = ModuleBuild(
            name=name,
            descriptor=descriptor,
            rules=rules,
            triple=triple,
            env_key=env_key,
            intermediate=self.paths.intermediate_build
            / str(triple.platform)
            / str(triple.arch)
            / str(triple.configuration)
            / env_name
            / name,
            roots=roots,
            cpp_sources=cpp_sources,
            c_sources=c_sources,
            rc_sources=rc_sources,
            overridden_sources=overridden,
            unity=unity,
            unity_exclude={_first_existing(roots, p).resolve() for p in rules.unity_exclude},
            nested_module_dirs=[d for root in roots for d in self.discovered.nested_module_dirs[root]],
            pch=pch,
            include_paths=_unique(include_paths),
            system_include_paths=_unique(system_include_paths),
            definitions=_unique([*global_definitions, *target_definitions, *api, *definitions]),
            warnings_as_errors=(
                self.settings.warnings_as_errors if rules.warnings_as_errors is None else rules.warnings_as_errors
            ),
            visible_modules=visible_modules,
            visible_third_party=visible_third_party,
            generated=generated,
            generated_dependencies=generated_dependencies,
        )
        self._modules[node] = build
        return build

    def _has_generated_code(self, name: str, ctx: BuildContext) -> bool:
        """Return whether module *name* is processed by the header tool: it is Core or sees it."""
        return name == CODEGEN_MODULE or CODEGEN_MODULE in self._visible(name, ctx)

    def _generated_dir(self, name: str, triple: Triple) -> Path:
        return self.paths.generated_code / str(triple.platform) / str(triple.arch) / name

    def _roots(self, descriptor: ModuleDescriptor, ctx: BuildContext) -> list[Path]:
        return descriptor.roots(layer_stack(ctx.platform, ctx.arch))

    def _own_include_paths(self, roots: list[Path], rules: ModuleRules) -> list[Path]:
        """Return the include paths a module made of *roots* sees of itself.

        A root without ``Public/``, ``Internal/`` and ``Private/`` is seen whole.
        """
        include_paths: list[Path] = []
        for root in roots:
            own = [root / sub for sub in OWN_INCLUDE_DIRS if self._is_dir(root / sub)]
            include_paths += own or [root]
        include_paths += self._layered(roots, [*rules.public_include_paths, *rules.private_include_paths])
        return include_paths

    def _layered(self, roots: list[Path], entries: list[str]) -> list[Path]:
        """Return each of *entries* in every root that has it, or in the module directory if none does."""
        result: list[Path] = []
        for entry in entries:
            result += [root / entry for root in roots if self._is_dir(root / entry)] or [roots[-1] / entry]
        return result

    def _is_dir(self, path: Path) -> bool:
        if path not in self._existing_dirs:
            self._existing_dirs[path] = path.is_dir()
        return self._existing_dirs[path]

    def _module_sources(self, name: str, roots: list[Path], platform: Platform) -> tuple[list[Path], list[Path]]:
        """Return the sources of module *name* made of *roots*, and those replaced; walk each root once."""
        key = (name, tuple(roots), platform)
        if key not in self._sources:
            self._sources[key] = _collect_sources(roots, platform, self.discovered.module_dirs)
        return self._sources[key]

    def _global_definitions(self, ctx: BuildContext, triple: Triple, module: str) -> list[str]:
        configuration = self.settings.configurations[triple.configuration]
        definitions = [f"MARLIN_PLATFORM_{p.value.upper()}={int(p is triple.platform)}" for p in Platform]
        definitions += [f"MARLIN_ARCH_{a.value.upper()}={int(a is triple.arch)}" for a in Arch]
        definitions += [f"MARLIN_BUILD_{c.value.upper()}={int(c is triple.configuration)}" for c in Configuration]
        definitions += [
            f"MARLIN_MONOLITHIC={int(ctx.is_monolithic)}",
            f"MARLIN_ASSERTIONS={int(configuration.assertions)}",
            f'MARLIN_MODULE_NAME="{module}"',
            f'MARLIN_PLATFORM_LAYERS="{";".join(layer_stack(triple.platform, triple.arch))}"',
            f'MARLIN_PLATFORM_LAYER_TABLE="{layer_table(triple.platform, triple.arch)}"',
        ]
        if not configuration.assertions:
            definitions.append("NDEBUG=1")
        return definitions + self.graph.version.global_definitions() + configuration.definitions

    def _third_party_build(self, name: str, ctx: BuildContext) -> ThirdPartyBuild:
        """Return third-party library *name*, merged with its public dependencies.

        Overlays can hold the platform's headers and binaries: an include path is taken from every
        root that has it, and a library from the highest root that has it.
        """
        key = (name, ctx)
        if key not in self._third_party:
            descriptor = self.discovered.modules[name]
            rules = self.rules_for(name, ctx)
            assert isinstance(rules, ThirdPartyRules)
            _check_frameworks(rules, descriptor.rules_file, ctx)
            roots = self._roots(descriptor, ctx)
            include_paths = self._layered(roots, rules.public_include_paths)
            definitions = list(rules.public_definitions)
            system_libraries = list(rules.system_libraries)
            static_libraries = [_first_existing(roots, p) for p in rules.static_libraries]
            shared_libraries = [_shared(roots, s) for s in rules.shared_libraries]
            for dep in self._deps(name, ctx, public_only=True):
                inner = self._third_party_build(dep, ctx)
                include_paths += inner.include_paths
                definitions += inner.definitions
            self._third_party[key] = ThirdPartyBuild(
                name=name,
                include_paths=_unique(include_paths),
                definitions=_unique(definitions),
                system_libraries=system_libraries,
                frameworks=list(rules.frameworks),
                link_options=list(rules.link_options),
                static_libraries=static_libraries,
                shared_libraries=shared_libraries,
            )
        return self._third_party[key]


def _check_public_include_paths(descriptor: ModuleDescriptor, rules: ModuleRules) -> None:
    """Raise :exc:`GraphError` for a public include path that exposes non-public headers."""
    root = descriptor.dir.resolve()
    for entry in rules.public_include_paths:
        path = (descriptor.dir / entry).resolve()
        if path == root:
            raise GraphError(f"{descriptor.rules_file}: public_include_paths must not contain the module root")
        for hidden in ("Private", "Internal"):
            if path.is_relative_to(root / hidden):
                raise GraphError(
                    f"{descriptor.rules_file}: '{entry}' is under {hidden}/ and cannot be a public include path"
                )


def _check_frameworks(rules: ModuleRules | ThirdPartyRules, rules_file: Path, ctx: BuildContext) -> None:
    if rules.frameworks and ctx.platform is not Platform.macOS:
        raise GraphError(f"{rules_file}: frameworks are macOS-only; add them under `if ctx.platform is Platform.macOS`")


def _shared(roots: list[Path], library: SharedLibrary) -> tuple[Path, Path | None]:
    runtime = _first_existing(roots, library.runtime)
    return runtime, _first_existing(roots, library.link) if library.link else None


def _first_existing(roots: list[Path], relative: str) -> Path:
    """Return *relative* in the highest of *roots* that has it, or in the last root if none does."""
    return next((root / relative for root in roots if (root / relative).exists()), roots[-1] / relative)


def _api_definitions(module: str, visible_modules: list[str], ctx: BuildContext) -> list[str]:
    """Return the ``<MODULE>_API`` export and import definitions for *module*."""
    if ctx.is_monolithic:
        return [f"{m.upper()}_API=" for m in (module, *visible_modules)]
    if ctx.platform is Platform.Windows:
        export, import_ = "__declspec(dllexport)", "__declspec(dllimport)"
    else:
        export = import_ = '__attribute__((visibility("default")))'
    return [f"{module.upper()}_API={export}", *(f"{m.upper()}_API={import_}" for m in visible_modules)]


def _collect_sources(roots: list[Path], platform: Platform, module_dirs: set[Path]) -> tuple[list[Path], list[Path]]:
    """Return the sources below *roots* and the ones a higher root replaces.

    Other platforms' folders and nested modules are skipped. A source replaces those at the same
    path relative to a lower root; the result is sorted by that relative path.
    """
    other_platforms = {p.value for p in Platform if p is not platform}
    extensions = CPP_EXTENSIONS + C_EXTENSIONS
    extensions += OBJC_EXTENSIONS if platform is Platform.macOS else ()
    extensions += RC_EXTENSIONS if platform is Platform.Windows else ()
    chosen: dict[Path, Path] = {}
    overridden: list[Path] = []

    def walk(root: Path, path: Path) -> None:
        with os.scandir(path) as scan:
            entries = sorted((Path(e.path), e.is_dir()) for e in scan)
        for entry, is_dir in entries:
            if is_dir:
                if entry.name in other_platforms or entry in module_dirs or entry.name.startswith("."):
                    continue
                walk(root, entry)
            elif entry.suffix in extensions:
                relative = entry.relative_to(root)
                if relative in chosen:
                    overridden.append(entry)
                else:
                    chosen[relative] = entry

    for root in roots:
        walk(root, root)
    return [chosen[r] for r in sorted(chosen)], overridden


def _unique[T](items: list[T]) -> list[T]:
    return list(dict.fromkeys(items))
