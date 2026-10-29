#!/bin/sh
# Rebuilds Lib/Windows/X64/Zstd.lib from the zstd release named in Zstd.module.py with the LLVM toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted zstd-<version> source tree>
# Single-threaded, without the assembly decoder loop (the MSVC-ABI build cannot assemble it) and without the
# legacy formats; -Zl leaves the C runtime out of the objects, so every engine configuration can link the library.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$1/lib"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
OBJECTS=""
for FILE in "$SOURCE"/common/*.c "$SOURCE"/compress/*.c "$SOURCE"/decompress/*.c; do
    NAME="$(basename "$FILE" .c)"
    "$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -Zl -GS- -W0 -DNDEBUG -DZSTD_DISABLE_ASM=1 \
        -DZSTD_LEGACY_SUPPORT=0 -DZSTD_MULTITHREAD=0 "$(cygpath -m "$FILE")" -Fo"$(cygpath -m "$WORK/$NAME.obj")"
    OBJECTS="$OBJECTS $(cygpath -m "$WORK/$NAME.obj")"
done
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/Zstd.lib")" $OBJECTS
cp "$SOURCE/zstd.h" "$SOURCE/zstd_errors.h" "$HERE/Include/"
cp "$1/LICENSE" "$HERE/LICENSE"
rm -rf "$WORK"
