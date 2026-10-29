#!/bin/sh
# Rebuilds Lib/Windows/X64/Bc7EncRdo.lib from the bc7enc_rdo commit named in Bc7EncRdo.module.py with the LLVM
# toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted bc7enc_rdo source tree> <path to ispc.exe>
# bc7e.ispc is compiled for SSE2, SSE4 and AVX2 with ispc's run-time dispatch. The C++ is built against the release
# DLL runtime every engine configuration uses, without exceptions, together with MarlinBc7EncRdo.cpp beside this
# script, the C interface the engine calls. The library's command-line tool and PNG reader are left out.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$1"
ISPC="$2"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
"$ISPC" -O2 --arch=x86-64 --target-os=windows --target=sse2-i32x4,sse4-i32x4,avx2-i32x8 --opt=disable-assertions \
    --wno-perf "$(cygpath -m "$SOURCE/bc7e.ispc")" -o "$(cygpath -m "$WORK/bc7e.obj")" -h "$(cygpath -m "$WORK/bc7e_ispc.h")"
OBJECTS=""
for OBJECT in "$WORK"/bc7e*.obj; do
    OBJECTS="$OBJECTS $(cygpath -m "$OBJECT")"
done
for FILE in "$SOURCE"/bc7enc.cpp "$SOURCE"/bc7decomp.cpp "$SOURCE"/bc7decomp_ref.cpp "$SOURCE"/rgbcx.cpp \
    "$SOURCE"/ert.cpp "$SOURCE"/rdo_bc_encoder.cpp "$SOURCE"/utils.cpp "$SOURCE"/lodepng.cpp "$HERE"/Build/MarlinBc7EncRdo.cpp; do
    NAME="$(basename "$FILE" .cpp)"
    "$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -MD -EHs-c- -D_HAS_EXCEPTIONS=0 -GR- -GS- -W0 \
        -std:c++17 -DNDEBUG -DSUPPORT_BC7E=1 -D_CRT_SECURE_NO_WARNINGS -I"$(cygpath -m "$SOURCE")" \
        -I"$(cygpath -m "$WORK")" -I"$(cygpath -m "$HERE/Include")" "$(cygpath -m "$FILE")" \
        -Fo"$(cygpath -m "$WORK/$NAME.obj")"
    OBJECTS="$OBJECTS $(cygpath -m "$WORK/$NAME.obj")"
done
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/Bc7EncRdo.lib")" $OBJECTS
cp "$SOURCE/LICENSE" "$HERE/LICENSE"
rm -rf "$WORK"
