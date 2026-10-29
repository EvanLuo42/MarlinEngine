#!/bin/sh
# Rebuilds Lib/Windows/X64/Ufbx.lib from the ufbx release named in Ufbx.module.py with the LLVM toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted ufbx-<version> source tree>
# The single-file library ufbx.c with its default feature set; -Zl leaves the C runtime out of the object, so every
# engine configuration can link the library.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
"$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -Zl -GS- -W0 -DNDEBUG \
    "$(cygpath -m "$1/ufbx.c")" -Fo"$(cygpath -m "$WORK/ufbx.obj")"
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/Ufbx.lib")" "$(cygpath -m "$WORK/ufbx.obj")"
cp "$1/ufbx.h" "$HERE/Include/"
cp "$1/LICENSE" "$HERE/LICENSE"
rm -rf "$WORK"
