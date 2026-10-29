"""Locate the engine root and the well-known paths below it."""

import os
import sys
from pathlib import Path

from marlin.build.platforms import Host


def _is_engine_root(path: Path) -> bool:
    return (path / "Source").is_dir() and (path / "Extras" / "FASTBuild").is_dir()


def find_engine_root() -> Path:
    """Return the engine root.

    Use ``$MARLIN_ROOT`` if set, otherwise the first of the current directory and its parents that
    is an engine root, otherwise the root this toolkit is installed in. Raise :exc:`RuntimeError`
    if none is found, or if ``$MARLIN_ROOT`` names a folder that is not one: ``clean`` deletes below the root.
    """
    if env := os.environ.get("MARLIN_ROOT"):
        root = Path(env).resolve()
        if not _is_engine_root(root):
            raise RuntimeError(
                f"MARLIN_ROOT={env} is not a Marlin Engine root (it has no Source/ and Extras/FASTBuild/)."
            )
        return root
    cwd = Path.cwd().resolve()
    for candidate in (cwd, *cwd.parents):
        if _is_engine_root(candidate):
            return candidate
    fallback = Path(__file__).resolve().parents[4]
    if _is_engine_root(fallback):
        return fallback
    raise RuntimeError("Cannot locate the Marlin Engine root; set MARLIN_ROOT.")


def engine_id(root: Path) -> str:
    """Return the Id of a source engine at *root*, as the engine computes it at run time.

    It is ``Source-`` and the FNV-1a hash of the root path, '/'-separated, without a trailing '/' and, on Windows,
    lowercase; projects bind to it through the user's engine registry (Documentation/Architecture/02).
    """
    text = str(root.resolve()).replace("\\", "/").rstrip("/")
    if sys.platform == "win32":
        text = text.lower()
    value = 0x811C9DC5
    for byte in text.encode("utf-8"):
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return f"Source-{value:08x}"


class EnginePaths:
    """The well-known paths of the engine at *root*."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.source = root / "Source"
        self.build = root / "Build"
        self.config = root / "Config"
        self.extras = root / "Extras"
        self.platforms = root / "Platforms"
        self.intermediate = root / "Intermediate"
        self.binaries = root / "Binaries"
        self.saved = root / "Saved"

    def resolve(self, path: str | Path) -> Path:
        """Return *path* relative to the engine root, or unchanged if it is absolute."""
        path = Path(path)
        return path if path.is_absolute() else self.root / path

    @property
    def host_is_windows(self) -> bool:
        return sys.platform == "win32"

    def tool_executable(self, tool: str, name: str) -> Path:
        """Return the host's build of program *name* of the tool in ``Extras/<tool>/``.

        Tools keep one folder per platform and arch, ``Extras/<tool>/<Platform>/<Arch>/``, like the binaries of
        third-party modules; *name* has no extension.
        """
        host = Host.detect()
        return (
            self.extras
            / tool
            / host.platform.value
            / host.arch.value
            / (name + (".exe" if self.host_is_windows else ""))
        )

    @property
    def fbuild_exe(self) -> Path:
        return self.tool_executable("FASTBuild", "FBuild")

    @property
    def build_settings_files(self) -> list[Path]:
        """The settings files, lowest priority first; the .local file is per machine."""
        return [self.build / "BuildSettings.toml", self.build / "BuildSettings.local.toml"]

    @property
    def presets(self) -> Path:
        return self.build / "Presets"

    @property
    def version_file(self) -> Path:
        return self.build / "Version.json"

    @property
    def intermediate_build(self) -> Path:
        return self.intermediate / "Build"

    @property
    def generated_headers(self) -> Path:
        """The headers that generate writes; they sit next to the .bff and go away with it."""
        return self.intermediate / "FASTBuild" / "Generated"

    @property
    def generated_code(self) -> Path:
        """What MarlinHeaderTool writes, under ``<Platform>/<Arch>/<Module>``."""
        return self.intermediate / "Generated"

    @property
    def generated_bff(self) -> Path:
        return self.intermediate / "FASTBuild" / "fbuild.bff"

    @property
    def unity_isolate_list(self) -> Path:
        """The sources ``marlin build`` takes out of their unity files, one per line (UnityInputIsolateListFile)."""
        return self.intermediate / "FASTBuild" / "UnityIsolate.txt"

    @property
    def generated_manifest(self) -> Path:
        return self.intermediate / "FASTBuild" / "Generated.json"
