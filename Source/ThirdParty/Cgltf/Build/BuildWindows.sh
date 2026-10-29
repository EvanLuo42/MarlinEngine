#!/bin/sh
# Rebuilds Lib/Windows/X64/Cgltf.lib from the cgltf release named in Cgltf.module.py with the LLVM toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted cgltf-<version> source tree>
# The reader of the single-header library, compiled once from Cgltf.c beside this script so the engine never defines
# CGLTF_IMPLEMENTATION; -Zl leaves the C runtime out of the object, so every engine configuration can link the library.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
"$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -Zl -GS- -W0 -DNDEBUG -D_CRT_SECURE_NO_WARNINGS \
    -I"$(cygpath -m "$1")" "$(cygpath -m "$HERE/Build/Cgltf.c")" -Fo"$(cygpath -m "$WORK/Cgltf.obj")"
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/Cgltf.lib")" "$(cygpath -m "$WORK/Cgltf.obj")"
cp "$1/cgltf.h" "$HERE/Include/"
cp "$1/LICENSE" "$HERE/LICENSE"
rm -rf "$WORK"
