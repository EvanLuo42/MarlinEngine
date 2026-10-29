"""Find Visual Studio installations and choose the one the solution targets.

The version in the solution header decides which Visual Studio opens the solution when it is
double-clicked; the project files open in any version.
"""

import json
import os
import subprocess
from dataclasses import dataclass
from pathlib import Path

YEARS = {"2019": 16, "2022": 17, "2026": 18}


@dataclass(frozen=True)
class VisualStudio:
    """A Visual Studio release; *version* is ``"<major>.0"`` when it is not installed."""

    year: str
    version: str
    installed: bool
    path: Path | None = None

    def has_platform_toolset(self, vs_platform: str, toolset: str) -> bool:
        """Return whether this install has *toolset*, such as ``"ClangCL"``, for *vs_platform*."""
        if self.path is None:
            return False
        pattern = f"*/Platforms/{vs_platform}/PlatformToolsets/{toolset}"
        return any((self.path / "MSBuild" / "Microsoft" / "VC").glob(pattern))


class VisualStudioError(Exception):
    """Raised when no Visual Studio matches the setting."""


def resolve(setting: str) -> VisualStudio:
    """Return the Visual Studio for *setting*, ``"auto"`` or a release year such as ``"2026"``.

    ``"auto"`` picks the newest complete installation. A year that is not installed still yields a
    version, so the solution opens in that release on other machines.
    """
    installed = _installed()
    if setting == "auto":
        if not installed:
            raise VisualStudioError(
                "no complete Visual Studio installation found; set solution.visual_studio to a year"
            )
        return max(installed, key=lambda vs: _version_key(vs.version))
    major = YEARS[setting]
    matching = [vs for vs in installed if _version_key(vs.version)[0] == major]
    if matching:
        return max(matching, key=lambda vs: _version_key(vs.version))
    return VisualStudio(setting, f"{major}.0", installed=False)


def vswhere(*args: str) -> list[dict]:
    """Return vswhere's JSON output for *args*, or an empty list if vswhere is missing or fails.

    Without ``-all``, only complete installations are listed.
    """
    exe = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    exe = exe / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not exe.is_file():
        return []
    result = subprocess.run(
        [exe, "-products", "*", *args, "-format", "json", "-utf8"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if result.returncode != 0:
        return []
    return json.loads(result.stdout or "[]")


def _installed() -> list[VisualStudio]:
    by_major = {major: year for year, major in YEARS.items()}
    instances = []
    for instance in vswhere():
        version = instance.get("installationVersion", "")
        major = _version_key(version)[0]
        if major in by_major:
            path = Path(instance["installationPath"]) if instance.get("installationPath") else None
            instances.append(VisualStudio(by_major[major], version, installed=True, path=path))
    return instances


def _version_key(version: str) -> tuple[int, ...]:
    try:
        return tuple(int(part) for part in version.split("."))
    except ValueError:
        return (0,)
