#!/bin/sh
# Rebuilds Lib/Windows/X64/Lz4.lib from the lz4 release named in Lz4.module.py with the LLVM toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted lz4-<version> source tree>
# -Zl leaves the C runtime out of the objects, so every engine configuration can link the library.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$(cygpath -m "$1/lib")"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
for FILE in lz4 lz4hc; do
    "$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -Zl -GS- -W0 -DNDEBUG \
        "$SOURCE/$FILE.c" -Fo"$(cygpath -m "$WORK/$FILE.obj")"
done
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/Lz4.lib")" \
    "$(cygpath -m "$WORK/lz4.obj")" "$(cygpath -m "$WORK/lz4hc.obj")"
cp "$1/lib/lz4.h" "$1/lib/lz4hc.h" "$HERE/Include/"
cp "$1/lib/LICENSE" "$HERE/LICENSE"
rm -rf "$WORK"
