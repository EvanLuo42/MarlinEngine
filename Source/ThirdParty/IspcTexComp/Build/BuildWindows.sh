#!/bin/sh
# Rebuilds Lib/Windows/X64/IspcTexComp.lib from the ISPCTextureCompressor commit named in IspcTexComp.module.py with
# the LLVM toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted ISPCTextureCompressor source tree> <path to ispc.exe>
# The kernels are compiled for SSE2, SSE4 and AVX2 with ispc's run-time dispatch; the C++ against the release DLL
# runtime every engine configuration uses. The header is the library's.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$1/ispc_texcomp"
ISPC="$2"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
for KERNEL in kernel kernel_astc; do
    "$ISPC" -O2 --arch=x86-64 --target-os=windows --target=sse2-i32x4,sse4-i32x4,avx2-i32x8 \
        --opt=disable-assertions --wno-perf "$(cygpath -m "$SOURCE/$KERNEL.ispc")" -o "$(cygpath -m "$WORK/$KERNEL.obj")" \
        -h "$(cygpath -m "$WORK/${KERNEL}_ispc.h")"
done
OBJECTS=""
for OBJECT in "$WORK"/kernel*.obj; do
    OBJECTS="$OBJECTS $(cygpath -m "$OBJECT")"
done
for FILE in "$SOURCE"/ispc_texcomp.cpp "$SOURCE"/ispc_texcomp_astc.cpp; do
    NAME="$(basename "$FILE" .cpp)"
    "$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -MD -EHs-c- -D_HAS_EXCEPTIONS=0 -GR- -GS- -W0 \
        -DNDEBUG -I"$(cygpath -m "$WORK")" "$(cygpath -m "$FILE")" -Fo"$(cygpath -m "$WORK/$NAME.obj")"
    OBJECTS="$OBJECTS $(cygpath -m "$WORK/$NAME.obj")"
done
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/IspcTexComp.lib")" $OBJECTS
cp "$SOURCE/ispc_texcomp.h" "$HERE/Include/"
cp "$1/license.txt" "$HERE/LICENSE"
rm -rf "$WORK"
