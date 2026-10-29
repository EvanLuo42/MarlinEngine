"""Decide which build combinations are valid on this host.

There is no cross compilation: a desktop platform is built only on itself, with the host's arch.
Consoles are built from Windows with their vendor SDKs.

Each platform's code is spread over an ordered stack of layers, one folder per layer under
``Platforms/``: the platform's own layers, then its arch, then ``Generic``. See
:mod:`marlin.build.discovery` for how a layer adds to the modules of the layers below it.
"""

import platform as _platform
import sys
from dataclasses import dataclass

from marlin.build.model import Arch, Configuration, Platform, TargetType


@dataclass(frozen=True)
class Host:
    """The platform and arch this process runs on."""

    platform: Platform
    arch: Arch

    @staticmethod
    def detect() -> Host:
        """Return the current host."""
        match sys.platform:
            case "win32":
                host_platform = Platform.Windows
            case "darwin":
                host_platform = Platform.macOS
            case _:
                host_platform = Platform.Linux
        machine = _platform.machine().lower()
        arch = Arch.Arm64 if machine in ("arm64", "aarch64") else Arch.X64
        return Host(host_platform, arch)


@dataclass(frozen=True)
class PlatformTraits:
    """What a platform supports.

    *hosts* are the platforms it can be built from. *vs_platforms* maps each arch to the Visual
    Studio project platform used for it. *layers* are the platform's own layers, most specific
    first; :func:`layer_stack` adds the arch and ``Generic`` layers below them.
    """

    architectures: tuple[Arch, ...]
    target_types: frozenset[TargetType]
    hosts: frozenset[Platform]
    vs_platforms: dict[Arch, str]
    layers: tuple[str, ...]


_ALL_TYPES = frozenset(TargetType)
_CONSOLE_TYPES = frozenset({TargetType.Game})

TRAITS: dict[Platform, PlatformTraits] = {
    Platform.Windows: PlatformTraits(
        (Arch.X64, Arch.Arm64),
        _ALL_TYPES,
        frozenset({Platform.Windows}),
        {Arch.X64: "x64", Arch.Arm64: "ARM64"},
        ("Windows", "Microsoft"),
    ),
    Platform.macOS: PlatformTraits(
        (Arch.Arm64, Arch.X64), _ALL_TYPES, frozenset({Platform.macOS}), {}, ("macOS", "Apple", "Posix")
    ),
    Platform.Linux: PlatformTraits(
        (Arch.X64, Arch.Arm64), _ALL_TYPES, frozenset({Platform.Linux}), {}, ("Linux", "Posix")
    ),
    Platform.Switch2: PlatformTraits(
        (Arch.Arm64,), _CONSOLE_TYPES, frozenset({Platform.Windows}), {Arch.Arm64: "NX64"}, ("Switch2",)
    ),
    Platform.PS5: PlatformTraits(
        (Arch.X64,), _CONSOLE_TYPES, frozenset({Platform.Windows}), {Arch.X64: "Prospero"}, ("PS5",)
    ),
    Platform.Xbox: PlatformTraits(
        (Arch.X64,),
        _CONSOLE_TYPES,
        frozenset({Platform.Windows}),
        {Arch.X64: "Gaming.Xbox.Scarlett.x64"},
        ("Xbox", "Microsoft"),
    ),
}

GENERIC_LAYER = "Generic"

LAYERS: frozenset[str] = frozenset(
    {GENERIC_LAYER, *(a.value for a in Arch), *(layer for t in TRAITS.values() for layer in t.layers)}
)
"""Every layer name; each folder under ``Platforms/`` must be one of them."""


def layer_stack(platform: Platform, arch: Arch) -> tuple[str, ...]:
    """Return the layers of *platform* on *arch*, most specific first."""
    return (*TRAITS[platform].layers, arch.value, GENERIC_LAYER)


def layer_table(platform: Platform, arch: Arch) -> str:
    """Return every platform's layer stack as ``Name:Layer;Layer|Name:...``, for Config to read other platforms.

    *platform* uses *arch*; the others use their first arch.
    """
    entries = []
    for other, traits in TRAITS.items():
        other_arch = arch if other is platform else traits.architectures[0]
        entries.append(f"{other.value}:{';'.join(layer_stack(other, other_arch))}")
    return "|".join(entries)


def invalid_reason(
    host: Host, platform: Platform, arch: Arch, configuration: Configuration, target_type: TargetType
) -> str | None:
    """Return why a combination can never be built on *host*, or ``None`` if it can.

    Toolchain availability and per-target restrictions are checked separately.
    """
    traits = TRAITS[platform]
    if arch not in traits.architectures:
        return f"{platform} has no {arch} variant"
    if target_type not in traits.target_types:
        return f"{target_type} targets are not supported on {platform}"
    if target_type is TargetType.Editor and configuration is Configuration.Shipping:
        return "the editor has no Shipping configuration"
    if host.platform not in traits.hosts:
        return f"{platform} cannot be built from a {host.platform} host"
    if platform.is_desktop and arch is not host.arch:
        return f"cross-architecture builds are not supported ({host.arch} host)"
    return None
