#!/bin/sh
# Rebuilds Lib/Windows/X64/Md4c.lib from the md4c release named in Md4c.module.py with the LLVM toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted md4c-release-<version> source tree>
# Only the parser (md4c.c) is built: MarlinDoc renders HTML itself, so md4c-html is left out.
# -Zl leaves the C runtime out of the objects, so every engine configuration can link the library.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$(cygpath -m "$1/src")"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
"$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -Zl -GS- -W0 -DNDEBUG \
    "$SOURCE/md4c.c" -Fo"$(cygpath -m "$WORK/md4c.obj")"
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/Md4c.lib")" "$(cygpath -m "$WORK/md4c.obj")"
cp "$1/src/md4c.h" "$HERE/Include/"
cp "$1/LICENSE.md" "$HERE/LICENSE"
rm -rf "$WORK"
