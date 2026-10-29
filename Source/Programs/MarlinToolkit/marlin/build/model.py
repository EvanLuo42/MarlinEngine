"""Define the platforms, architectures, configurations and target types of the build."""

from dataclasses import dataclass
from enum import StrEnum


class Platform(StrEnum):
    Windows = "Windows"
    macOS = "macOS"
    Linux = "Linux"
    Switch2 = "Switch2"
    PS5 = "PS5"
    Xbox = "Xbox"

    @property
    def is_desktop(self) -> bool:
        return self in (Platform.Windows, Platform.macOS, Platform.Linux)

    @property
    def is_console(self) -> bool:
        return not self.is_desktop


class Arch(StrEnum):
    X64 = "X64"
    Arm64 = "Arm64"


class Configuration(StrEnum):
    Debug = "Debug"
    Development = "Development"
    Profile = "Profile"
    Shipping = "Shipping"


class TargetType(StrEnum):
    Game = "Game"
    Editor = "Editor"
    Server = "Server"
    Program = "Program"


class LinkMode(StrEnum):
    Monolithic = "monolithic"
    Modular = "modular"


class Optimize(StrEnum):
    none = "none"
    size = "size"
    speed = "speed"


@dataclass(frozen=True, order=True)
class Triple:
    """A platform, arch and configuration, written as ``Windows-X64-Development``."""

    platform: Platform
    arch: Arch
    configuration: Configuration

    def __str__(self) -> str:
        return f"{self.platform}-{self.arch}-{self.configuration}"


def parse_enum[E: StrEnum](enum: type[E], text: str) -> E:
    """Return the member of *enum* named *text*, ignoring case; raise :exc:`ValueError` otherwise."""
    for member in enum:
        if member.value.lower() == text.lower():
            return member
    choices = ", ".join(member.value for member in enum)
    raise ValueError(f"unknown {enum.__name__} '{text}' (expected one of: {choices})")
