"""Implement ``marlin symbols``: copy built binaries and their PDBs into a symbol store.

MarlinCrashServer reads the store to walk and symbolize the stacks of uploaded minidumps
(Documentation/Architecture/03 §7.1). The layout is the one ``CrashAnalyzer::GetImagePath`` and
``GetDebugFilePath`` expect, the same as Microsoft's symbol servers use:

- ``<store>/<image>/<TimeDateStamp:08X><SizeOfImage:X>/<image>``
- ``<store>/<pdb>/<GUID><Age:X>/<pdb>``

Files already in the store are left alone, so the command can run after every build.
"""

import argparse
import shutil
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from marlin.build.model import Configuration, Platform, parse_enum
from marlin.build.platforms import Host
from marlin.paths import EnginePaths

# RECOVERY-DECISION D13: options (-c, --store defaulting to Saved/SymbolStore, -n) and Windows-only scope chosen.
IMAGE_SUFFIXES = (".exe", ".dll")
DEBUG_DIRECTORY = 6
CODEVIEW = 2
PDB70_SIGNATURE = b"RSDS"


@dataclass(frozen=True)
class ImageKey:
    """The identity of a PE image and the PDB it names."""

    time_stamp: int
    image_size: int
    pdb_name: str | None = None
    pdb_guid: bytes | None = None
    pdb_age: int = 0

    @property
    def image_key(self) -> str:
        return f"{self.time_stamp:08X}{self.image_size:X}"

    @property
    def pdb_key(self) -> str | None:
        if self.pdb_guid is None:
            return None
        # The GUID's first three fields are little-endian in the file; the key writes them big-endian.
        guid = self.pdb_guid
        ordered = guid[3::-1] + guid[5:3:-1] + guid[7:5:-1] + guid[8:]
        return f"{ordered.hex().upper()}{self.pdb_age:X}"


def register(subparsers: argparse._SubParsersAction) -> None:
    """Add the ``symbols`` subcommand to *subparsers*."""
    parser = subparsers.add_parser(
        "symbols",
        help="Copy built binaries and PDBs into a symbol store for crash analysis.",
        description="Copy the executables, DLLs and PDBs of a build into a symbol store laid out by image time "
        "stamp and size, and by PDB GUID and age, for MarlinCrashServer (Engine.CrashServer.SymbolStore). Files "
        "already in the store are kept.",
    )
    parser.add_argument("-c", "--configuration", default="Development", help="build configuration to store")
    parser.add_argument(
        "--store", default="Saved/SymbolStore", help="symbol store folder, relative to the engine root if relative"
    )
    parser.add_argument("-n", "--dry-run", action="store_true", help="list what would be copied")
    parser.set_defaults(handler=run)


def run(args: argparse.Namespace, paths: EnginePaths) -> int:
    """Copy the binaries of the host build of *args.configuration* into the store; return the exit code."""
    try:
        configuration = parse_enum(Configuration, args.configuration)
    except ValueError as error:
        print(f"marlin symbols: {error}", file=sys.stderr)
        return 1
    host = Host.detect()
    if host.platform is not Platform.Windows:
        print("marlin symbols: only Windows binaries are supported", file=sys.stderr)
        return 1
    binaries = paths.binaries / str(host.platform) / str(host.arch) / str(configuration)
    if not binaries.is_dir():
        print(f"marlin symbols: {binaries} does not exist; build {configuration} first", file=sys.stderr)
        return 1
    store = paths.resolve(args.store)
    copied = 0
    failures = 0
    for image in sorted(p for p in binaries.iterdir() if p.suffix.lower() in IMAGE_SUFFIXES and p.is_file()):
        try:
            key = read_image_key(image)
        except (OSError, ValueError) as error:
            print(f"marlin symbols: skipping {image.name}: {error}", file=sys.stderr)
            failures += 1
            continue
        files = [(image, store / image.name / key.image_key / image.name)]
        if key.pdb_name is not None and key.pdb_key is not None:
            pdb = binaries / key.pdb_name
            if pdb.is_file():
                files.append((pdb, store / key.pdb_name / key.pdb_key / key.pdb_name))
        for source, target in files:
            if target.exists():
                continue
            if args.dry_run:
                print(f"would copy {source.name} to {target.relative_to(store)}")
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                print(f"copied {source.name} to {target.relative_to(store)}")
            copied += 1
    print(f"{'Would copy' if args.dry_run else 'Copied'} {copied} files to {store}.")
    return 1 if failures else 0


def read_image_key(path: Path) -> ImageKey:
    """Return the time stamp and size of the PE32+ image at *path*, with the PDB its CodeView record names."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError("not a PE image")
    nt = struct.unpack_from("<I", data, 0x3C)[0]
    if data[nt : nt + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    section_count, time_stamp = struct.unpack_from("<HI", data, nt + 6)
    optional_size = struct.unpack_from("<H", data, nt + 20)[0]
    optional = nt + 24
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("not a PE32+ image")
    image_size = struct.unpack_from("<I", data, optional + 56)[0]
    directory_count = struct.unpack_from("<I", data, optional + 108)[0]
    if directory_count <= DEBUG_DIRECTORY:
        return ImageKey(time_stamp, image_size)
    debug_rva, debug_size = struct.unpack_from("<II", data, optional + 112 + DEBUG_DIRECTORY * 8)
    sections = [struct.unpack_from("<IIII", data, optional + optional_size + i * 40 + 8) for i in range(section_count)]

    def file_offset(rva: int) -> int | None:
        for virtual_size, virtual_address, raw_size, raw_offset in sections:
            if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
                return raw_offset + rva - virtual_address
        return None

    start = file_offset(debug_rva)
    if start is None:
        return ImageKey(time_stamp, image_size)
    for entry in range(start, start + debug_size, 28):
        kind, size, _, pointer = struct.unpack_from("<IIII", data, entry + 12)
        record = data[pointer : pointer + size]
        if kind != CODEVIEW or record[:4] != PDB70_SIGNATURE or len(record) < 25:
            continue
        guid = record[4:20]
        age = struct.unpack_from("<I", record, 20)[0]
        name = record[24:].split(b"\0", 1)[0].decode("utf-8", errors="replace")
        return ImageKey(time_stamp, image_size, Path(name.replace("\\", "/")).name, guid, age)
    return ImageKey(time_stamp, image_size)
