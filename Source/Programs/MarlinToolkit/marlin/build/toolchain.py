"""Compiler and linker command lines for each platform.

Desktop platforms use LLVM: clang++ with the platform's target triple, linked by lld-link on Windows
and by the clang driver elsewhere. Console toolchains come from their vendor SDKs.
"""

import os
import re
import shutil
import subprocess
import sys
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from pathlib import Path
from typing import Literal

from marlin.build.model import Arch, Optimize, Platform
from marlin.build.settings import BuildSettings
from marlin.build.visualstudio import vswhere


@dataclass
class CompileSpec:
    """How one set of sources is compiled.

    *pch* is the precompiled header to use, given before the *force_includes*. *extra* holds the
    module's own ``compile_options``, given after the engine's so they take precedence. C++ is
    compiled without exceptions and RTTI unless *exceptions* or *rtti* is set.
    """

    language: Literal["c", "c++"]
    standard: str
    optimize: Optimize
    lto: bool
    debug_info: bool
    warnings_as_errors: bool
    sanitizers: list[str]
    include_paths: list[Path]
    system_include_paths: list[Path]
    force_includes: list[Path]
    pch: Path | None = None
    exceptions: bool = False
    rtti: bool = False
    extra: list[str] = field(default_factory=list)


@dataclass
class LinkSpec:
    """How one executable or shared library is linked."""

    shared: bool
    console: bool
    optimize: Optimize
    lto: bool
    debug_info: bool
    sanitizers: list[str]
    system_libraries: list[str]
    frameworks: list[str] = field(default_factory=list)
    import_library: Path | None = None
    pdb: Path | None = None
    install_name: str | None = None
    extra: list[str] = field(default_factory=list)


class Toolchain(ABC):
    """Abstract compiler and linker for one platform and arch.

    Subclasses provide the tool paths and the FASTBuild option strings, in which ``%1`` and ``%2``
    stand for the inputs and the output.
    """

    def __init__(self, platform: Platform, arch: Arch) -> None:
        self.platform = platform
        self.arch = arch

    @property
    def name(self) -> str:
        return f"{type(self).__name__}-{self.platform}-{self.arch}"

    @property
    @abstractmethod
    def compiler(self) -> Path: ...

    @property
    @abstractmethod
    def linker(self) -> Path: ...

    @property
    def compiler_family(self) -> str:
        return "clang"

    @property
    def linker_type(self) -> str:
        return "gcc"

    @property
    def compiler_extra_files(self) -> list[Path]:
        return []

    @property
    @abstractmethod
    def version(self) -> str: ...

    obj_ext = ".o"
    pch_ext = ".pch"
    res_ext = ".res"

    @property
    def resource_compiler(self) -> Path | None:
        """The compiler for Windows .rc resource scripts, or ``None`` if the platform has none."""
        return None

    def resource_options(self, include_paths: list[Path], definitions: list[str]) -> list[str]:
        """Return the options that compile the .rc script ``%1`` into ``%2``."""
        raise NotImplementedError(f"{self.name} has no resource compiler")

    def exe_name(self, name: str) -> str:
        return name

    def shared_lib_name(self, name: str) -> str:
        return f"lib{name}.so"

    def import_lib_name(self, name: str) -> str | None:
        return None

    def debug_file_name(self, name: str) -> str | None:
        return None

    def sanitizer_runtimes(self, sanitizers: list[str]) -> list[Path]:
        """Return the shared libraries that binaries built with *sanitizers* load from their own directory."""
        return []

    @abstractmethod
    def compile_options(self, spec: CompileSpec) -> list[str]:
        """Return the ``.CompilerOptions`` that compile ``%1`` into ``%2``."""

    @abstractmethod
    def pch_create_options(self, spec: CompileSpec) -> list[str]:
        """Return the ``.PCHOptions`` that precompile the header ``%1`` into ``%2``."""

    @abstractmethod
    def link_options(self, spec: LinkSpec) -> list[str]:
        """Return the ``.LinkerOptions`` that link the inputs ``%1`` into ``%2``."""


class ToolchainUnavailable(Exception):
    """Raised when a platform's compiler or SDK cannot be found."""


def make_toolchain(platform: Platform, arch: Arch, settings: BuildSettings, root: Path) -> Toolchain:
    """Return the toolchain for *platform* and *arch*, or raise :exc:`ToolchainUnavailable`."""
    if platform.is_console:
        raise ToolchainUnavailable(f"{platform} SDK integration is not available (Platforms/{platform})")
    llvm = find_llvm(settings, root)
    toolchain: _Clang
    match platform:
        case Platform.Windows:
            toolchain = ClangWindows(platform, arch, llvm, find_msvc(arch))
        case Platform.Linux:
            toolchain = ClangLinux(platform, arch, llvm)
        case Platform.macOS:
            toolchain = ClangMac(platform, arch, llvm)
        case _:
            raise ToolchainUnavailable(f"no toolchain for {platform}")
    _check_llvm_version(toolchain)
    return toolchain


MIN_LLVM_VERSION = (18, 0)


def _check_llvm_version(toolchain: _Clang) -> None:
    """Raise :exc:`ToolchainUnavailable` if the toolchain's LLVM is older than ``MIN_LLVM_VERSION``.

    The minimum keeps C++20 support the same on every machine. A version banner that cannot be
    parsed, such as a vendor fork's, is accepted.
    """
    match = re.search(r"clang version (\d+)\.(\d+)", toolchain.version)
    if match and (int(match[1]), int(match[2])) < MIN_LLVM_VERSION:
        minimum = ".".join(map(str, MIN_LLVM_VERSION))
        raise ToolchainUnavailable(
            f"LLVM {match[1]}.{match[2]} at {toolchain.llvm_root} is older than the minimum {minimum}; "
            "install a newer LLVM or point toolchain.llvm_root at one"
        )


def find_llvm(settings: BuildSettings, root: Path) -> Path:
    """Return the LLVM install root.

    Look at ``toolchain.llvm_root``, then ``$LLVM_ROOT``, then the default install location and
    ``PATH``. Raise :exc:`ToolchainUnavailable` if none has clang++.
    """
    exe = ".exe" if sys.platform == "win32" else ""
    candidates: list[Path] = []
    if settings.toolchain.llvm_root:
        candidates.append(root / settings.toolchain.llvm_root)
    elif env := os.environ.get("LLVM_ROOT"):
        candidates.append(Path(env))
    else:
        if sys.platform == "win32":
            candidates.append(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM")
        elif sys.platform == "darwin":
            candidates += [Path("/opt/homebrew/opt/llvm"), Path("/usr/local/opt/llvm")]
        if found := shutil.which("clang++"):
            candidates.append(Path(found).resolve().parent.parent)
    for candidate in candidates:
        if (candidate / "bin" / f"clang++{exe}").is_file():
            return candidate.resolve()
    searched = ", ".join(str(c) for c in candidates) or "nothing"
    raise ToolchainUnavailable(f"LLVM not found (searched {searched}); set toolchain.llvm_root")


@dataclass(frozen=True)
class MsvcEnvironment:
    """The VC++ STL/CRT and Windows SDK that clang and lld-link build against.

    The paths are passed explicitly instead of left to clang's own lookup, which only checks the
    newest Visual Studio and fails when that install lacks the C++ build tools. *vc_tools* is a
    ``VC/Tools/MSVC/<version>`` directory and *sdk_root* a ``Windows Kits/10`` directory.
    """

    vc_tools: Path
    sdk_root: Path
    sdk_version: str

    @property
    def include_paths(self) -> list[Path]:
        sdk = self.sdk_root / "Include" / self.sdk_version
        return [self.vc_tools / "include", sdk / "ucrt", sdk / "shared", sdk / "um", sdk / "winrt"]

    def link_options(self) -> list[str]:
        """Return the lld-link options that locate the libraries; /MACHINE picks the arch."""
        return [
            f"/vctoolsdir:{_q(self.vc_tools)}",
            f"/winsdkdir:{_q(self.sdk_root)}",
            f"/winsdkversion:{self.sdk_version}",
        ]


_VC_TOOLS_COMPONENTS = {
    Arch.X64: "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    Arch.Arm64: "Microsoft.VisualStudio.Component.VC.Tools.ARM64",
}


def find_msvc(arch: Arch) -> MsvcEnvironment:
    """Return the VC++ tools and Windows SDK to build *arch* with.

    A developer command prompt's ``VCToolsInstallDir`` and ``WindowsSdkDir`` take precedence.
    Otherwise use the newest Visual Studio that has the C++ build tools for *arch* and the newest
    complete Windows SDK.
    """
    return MsvcEnvironment(_find_vc_tools(arch), *_find_windows_sdk())


def _find_vc_tools(arch: Arch) -> Path:
    if env := os.environ.get("VCToolsInstallDir"):
        return Path(env)
    component = _VC_TOOLS_COMPONENTS[arch]
    for instance in vswhere("-latest", "-requires", component):
        vc = Path(instance["installationPath"]) / "VC"
        default = vc / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt"
        if default.is_file():
            tools = vc / "Tools" / "MSVC" / default.read_text(encoding="utf-8").strip()
            if (tools / "include").is_dir():
                return tools
    raise ToolchainUnavailable(f"no Visual Studio with {component} found; install it with the Visual Studio Installer")


def _find_windows_sdk() -> tuple[Path, str]:
    if (env := os.environ.get("WindowsSdkDir")) and (version := os.environ.get("WindowsSDKVersion")):
        return Path(env), version.rstrip("\\")
    root = _windows_kits_root()
    include = root / "Include"
    versions = [d.name for d in include.iterdir() if (d / "um" / "Windows.h").is_file()] if include.is_dir() else []
    if not versions:
        raise ToolchainUnavailable(f"no Windows SDK found in {root}; install one with the Visual Studio Installer")
    return root, max(versions, key=lambda v: tuple(int(p) for p in v.split(".") if p.isdigit()))


def _windows_kits_root() -> Path:
    if sys.platform == "win32":
        import winreg

        try:
            key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows Kits\Installed Roots")
            with key:
                return Path(winreg.QueryValueEx(key, "KitsRoot10")[0])
        except OSError:
            pass
    return Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10"


def _q(path: Path | str) -> str:
    return f'"{path}"'


def _sanitizer_names(sanitizers: list[str]) -> set[str]:
    """Return the sanitizers in *sanitizers*, whose entries may be comma-separated like ``-fsanitize``."""
    return {name.strip() for entry in sanitizers for name in entry.split(",")}


class _Clang(Toolchain):
    """Base class of the LLVM toolchains.

    *default_system_libraries* are linked into every binary and cover only what the C runtime needs.
    Other system libraries belong to the module that uses them, as its ``system_libraries``.
    """

    default_system_libraries: tuple[str, ...] = ()

    def __init__(self, platform: Platform, arch: Arch, llvm_root: Path) -> None:
        super().__init__(platform, arch)
        self.llvm_root = llvm_root
        self._exe = ".exe" if sys.platform == "win32" else ""
        self._version: str | None = None
        self._resource_dir: Path | None = None

    @property
    def name(self) -> str:
        return f"Clang-{self.platform}-{self.arch}"

    @property
    def compiler(self) -> Path:
        return self.llvm_root / "bin" / f"clang++{self._exe}"

    @property
    def linker(self) -> Path:
        return self.compiler

    @property
    def version(self) -> str:
        if self._version is None:
            output = subprocess.run([self.compiler, "--version"], capture_output=True, text=True, check=False).stdout
            self._version = output.splitlines()[0] if output else "unknown"
        return self._version

    @property
    def resource_dir(self) -> Path:
        """Clang's resource directory, which holds the sanitizer runtimes."""
        if self._resource_dir is None:
            output = subprocess.run(
                [self.compiler, "-print-resource-dir"], capture_output=True, text=True, check=False
            ).stdout.strip()
            self._resource_dir = Path(output) if output else self.llvm_root / "lib" / "clang"
        return self._resource_dir

    @property
    @abstractmethod
    def triple(self) -> str: ...

    def target_options(self) -> list[str]:
        return [f"--target={self.triple}"]

    def compile_options(self, spec: CompileSpec) -> list[str]:
        language = ["-x c"] if spec.language == "c" else []
        return [*language, '"%1"', '-o "%2"', "-c", *self._common_options(spec)]

    def pch_create_options(self, spec: CompileSpec) -> list[str]:
        """``-fno-pch-timestamp`` leaves the inputs' times out of the PCH: one from the cache was built against
        files with other times, and clang would reject it although FASTBuild checked the contents."""
        return [
            f"-x {spec.language}-header",
            '"%1"',
            '-o "%2"',
            "-c",
            "-Xclang -fno-pch-timestamp",
            *self._common_options(spec),
        ]

    def _common_options(self, spec: CompileSpec) -> list[str]:
        """Return the options shared by compiling and precompiling.

        ``-Wno-#pragma-messages`` silences the message FASTBuild's unity files print per source.
        The minimum ISA is x86-64-v3 (AVX2) on X64 and ARMv8.2-A on Arm64.
        """
        options = [*self.target_options(), f"-std={spec.standard}"]
        options.append("-march=x86-64-v3" if self.arch is Arch.X64 else "-march=armv8.2-a")
        if spec.language == "c++":
            options.append("-fexceptions" if spec.exceptions else "-fno-exceptions")
            options.append("-frtti" if spec.rtti else "-fno-rtti")
        options += {
            Optimize.none: ["-O0"],
            Optimize.size: ["-Os", "-ffunction-sections", "-fdata-sections"],
            Optimize.speed: ["-O2", "-ffunction-sections", "-fdata-sections"],
        }[spec.optimize]
        if spec.lto:
            options.append("-flto=thin")
        if spec.debug_info:
            options += self._debug_options()
        options += ["-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-#pragma-messages"]
        if spec.warnings_as_errors:
            options.append("-Werror")
        options += [f"-fsanitize={s}" for s in spec.sanitizers]
        options += self._sanitizer_compile_options(spec.sanitizers)
        options += self._platform_compile_options()
        options += spec.extra
        options += [f"-I{_q(p)}" for p in spec.include_paths]
        options += [f"-isystem {_q(p)}" for p in spec.system_include_paths]
        if spec.pch:
            options.append(f"-include-pch {_q(spec.pch)}")
        options += [f"-include {_q(p)}" for p in spec.force_includes]
        return options

    def _debug_options(self) -> list[str]:
        return ["-g"]

    def _platform_compile_options(self) -> list[str]:
        return []

    def _sanitizer_compile_options(self, sanitizers: list[str]) -> list[str]:
        return []

    def _system_libraries(self, spec: LinkSpec) -> list[str]:
        return list(dict.fromkeys([*self.default_system_libraries, *spec.system_libraries]))


class ClangWindows(_Clang):
    """clang++ and lld-link against the VC++ runtime and the Windows SDK.

    Every configuration uses the release DLL runtime, so prebuilt third-party libraries link in all
    of them. The runtime libraries come from the ``/DEFAULTLIB`` directives that
    ``-fms-runtime-lib`` puts into every object; kernel32, which the runtime's startup code
    imports, is listed explicitly. Executables enter through ``main`` whatever their subsystem.
    llvm-rc needs the SDK include paths given explicitly and reads scripts as UTF-8.

    The sanitizers are clang's own. lld-link is not the clang driver, so the libraries the driver would add are
    listed here: AddressSanitizer's DLL runtime with its thunk for the DLL C runtime, linked into every executable
    and DLL, and the runtime directory where UndefinedBehaviorSanitizer's objects ask for their static runtime.
    The VC++ STL's container annotations are turned off, since they need MSVC's own ASan library.
    """

    obj_ext = ".obj"
    default_system_libraries = ("kernel32",)

    def __init__(self, platform: Platform, arch: Arch, llvm_root: Path, msvc: MsvcEnvironment) -> None:
        super().__init__(platform, arch, llvm_root)
        self.msvc = msvc

    @property
    def triple(self) -> str:
        return f"{'x86_64' if self.arch is Arch.X64 else 'aarch64'}-pc-windows-msvc"

    @property
    def linker(self) -> Path:
        return self.llvm_root / "bin" / "lld-link.exe"

    @property
    def linker_type(self) -> str:
        return "msvc"

    def exe_name(self, name: str) -> str:
        return f"{name}.exe"

    def shared_lib_name(self, name: str) -> str:
        return f"{name}.dll"

    def import_lib_name(self, name: str) -> str | None:
        return f"{name}.lib"

    def debug_file_name(self, name: str) -> str | None:
        return f"{name}.pdb"

    def sanitizer_runtimes(self, sanitizers: list[str]) -> list[Path]:
        return [self._runtime_library("asan_dynamic", ".dll")] if "address" in _sanitizer_names(sanitizers) else []

    @property
    def _runtime_dir(self) -> Path:
        return self.resource_dir / "lib" / "windows"

    def _runtime_library(self, name: str, extension: str) -> Path:
        arch = "x86_64" if self.arch is Arch.X64 else "aarch64"
        return self._runtime_dir / f"clang_rt.{name}-{arch}{extension}"

    def _sanitizer_compile_options(self, sanitizers: list[str]) -> list[str]:
        return ["-D_DISABLE_STL_ANNOTATION"] if "address" in _sanitizer_names(sanitizers) else []

    def _sanitizer_link_options(self, sanitizers: list[str]) -> list[str]:
        if not sanitizers:
            return []
        options = [f"/LIBPATH:{_q(self._runtime_dir)}"]
        if "address" in _sanitizer_names(sanitizers):
            options += [
                _q(self._runtime_library("asan_dynamic", ".lib")),
                "/INCLUDE:__asan_seh_interceptor",
                f"/WHOLEARCHIVE:{_q(self._runtime_library('asan_dynamic_runtime_thunk', '.lib'))}",
            ]
        return options

    def _debug_options(self) -> list[str]:
        return ["-g", "-gcodeview"]

    def _platform_compile_options(self) -> list[str]:
        options = ["-fms-runtime-lib=dll", "-fdiagnostics-absolute-paths"]
        return options + [f"-isystem {_q(p)}" for p in self.msvc.include_paths]

    @property
    def resource_compiler(self) -> Path | None:
        return self.llvm_root / "bin" / "llvm-rc.exe"

    def resource_options(self, include_paths: list[Path], definitions: list[str]) -> list[str]:
        options = ["/nologo", "/C 65001"]
        options += [f"/I {_q(p)}" for p in [*include_paths, *self.msvc.include_paths]]
        options += [f"/D {d}" for d in definitions]
        return [*options, '/FO "%2"', '"%1"']

    def link_options(self, spec: LinkSpec) -> list[str]:
        machine = "X64" if self.arch is Arch.X64 else "ARM64"
        options = ["/NOLOGO", '/OUT:"%2"', '"%1"', f"/MACHINE:{machine}", "/INCREMENTAL:NO", "/Brepro"]
        if spec.shared:
            options.append("/DLL")
            if spec.import_library:
                options.append(f"/IMPLIB:{_q(spec.import_library)}")
        else:
            options += [f"/SUBSYSTEM:{'CONSOLE' if spec.console else 'WINDOWS'}", "/ENTRY:mainCRTStartup"]
        if spec.debug_info:
            options.append("/DEBUG:FULL")
            if spec.pdb:
                # Only the file name goes into the image, so a crash dump never carries the build machine's paths.
                options += [f"/PDB:{_q(spec.pdb)}", "/PDBALTPATH:%_PDB%"]
        else:
            options.append("/DEBUG:NONE")
        if spec.optimize is not Optimize.none:
            options += ["/OPT:REF", "/OPT:ICF"]
        options += self._sanitizer_link_options(spec.sanitizers)
        options += self.msvc.link_options()
        options += [f"{lib}.lib" for lib in self._system_libraries(spec)]
        options += spec.extra
        return options


class ClangLinux(_Clang):
    """clang++ and lld against glibc.

    libc is implicit and ``-pthread`` adds libpthread; libm and libdl are separate libraries before
    glibc 2.34.
    """

    default_system_libraries = ("m", "dl")

    @property
    def triple(self) -> str:
        return f"{'x86_64' if self.arch is Arch.X64 else 'aarch64'}-unknown-linux-gnu"

    def _platform_compile_options(self) -> list[str]:
        return ["-fPIC", "-fvisibility=hidden", "-pthread"]

    def link_options(self, spec: LinkSpec) -> list[str]:
        options = [*self.target_options(), "-fuse-ld=lld", '"%1"', '-o "%2"', "-pthread", "-Wl,-rpath,$ORIGIN"]
        if spec.shared:
            options += ["-shared", "-Wl,--no-undefined"]
        if spec.optimize is not Optimize.none:
            options.append("-Wl,--gc-sections")
        if spec.lto:
            options.append("-flto=thin")
        options += [f"-fsanitize={s}" for s in spec.sanitizers]
        options += [f"-l{lib}" for lib in self._system_libraries(spec)]
        options += spec.extra
        return options


class ClangMac(_Clang):
    """clang++ against the Xcode macOS SDK."""

    def __init__(self, platform: Platform, arch: Arch, llvm_root: Path) -> None:
        super().__init__(platform, arch, llvm_root)
        self._sdk: str | None = None

    @property
    def triple(self) -> str:
        return f"{'x86_64' if self.arch is Arch.X64 else 'arm64'}-apple-macos13.0"

    def shared_lib_name(self, name: str) -> str:
        return f"lib{name}.dylib"

    @property
    def sdk(self) -> str:
        if self._sdk is None:
            result = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=False)
            if result.returncode != 0:
                raise ToolchainUnavailable("macOS SDK not found (xcrun --show-sdk-path failed)")
            self._sdk = result.stdout.strip()
        return self._sdk

    def target_options(self) -> list[str]:
        return [f"--target={self.triple}", f"-isysroot {_q(self.sdk)}"]

    def _platform_compile_options(self) -> list[str]:
        return ["-fvisibility=hidden"]

    def link_options(self, spec: LinkSpec) -> list[str]:
        options = [*self.target_options(), '"%1"', '-o "%2"', "-Wl,-rpath,@executable_path"]
        if spec.shared:
            options.append("-dynamiclib")
            if spec.install_name:
                options.append(f"-install_name @rpath/{spec.install_name}")
        if spec.optimize is not Optimize.none:
            options.append("-Wl,-dead_strip")
        if spec.lto:
            options.append("-flto=thin")
        options += [f"-fsanitize={s}" for s in spec.sanitizers]
        options += [f"-l{lib}" for lib in self._system_libraries(spec)]
        options += [f"-framework {framework}" for framework in spec.frameworks]
        options += spec.extra
        return options
