#!/bin/sh
# Rebuilds Lib/Windows/X64/MeshOptimizer.lib from the meshoptimizer release named in MeshOptimizer.module.py with the
# LLVM toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted meshoptimizer-<version> source tree>
# The library and clusterlod, the cluster LOD DAG builder from its demo folder (ClusterLod.cpp beside this script holds its
# implementation), against the release DLL runtime every engine configuration uses and without exceptions.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$1/src"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
OBJECTS=""
for FILE in "$SOURCE"/*.cpp "$HERE"/Build/ClusterLod.cpp; do
    NAME="$(basename "$FILE" .cpp)"
    "$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -MD -EHs-c- -D_HAS_EXCEPTIONS=0 -GR- -GS- -W0 -DNDEBUG \
        -I"$(cygpath -m "$SOURCE")" -I"$(cygpath -m "$1/demo")" "$(cygpath -m "$FILE")" -Fo"$(cygpath -m "$WORK/$NAME.obj")"
    OBJECTS="$OBJECTS $(cygpath -m "$WORK/$NAME.obj")"
done
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/MeshOptimizer.lib")" $OBJECTS
cp "$SOURCE/meshoptimizer.h" "$1/demo/clusterlod.h" "$HERE/Include/"
cp "$1/LICENSE.md" "$HERE/LICENSE"
rm -rf "$WORK"
