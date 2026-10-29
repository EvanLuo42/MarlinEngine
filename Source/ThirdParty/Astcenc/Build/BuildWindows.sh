#!/bin/sh
# Rebuilds Lib/Windows/X64/Astcenc.lib from the astc-encoder release named in Astcenc.module.py with the LLVM
# toolchain, from Git Bash.
# Usage: BuildWindows.sh <path to an extracted astc-encoder-<version> source tree>
# The codec library alone, for AVX2 without gathers as the release's avx2 build, against the release DLL runtime
# every engine configuration uses and without exceptions. The header is the release's.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$1/Source"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
OBJECTS=""
for FILE in "$SOURCE"/astcenc_*.cpp; do
    NAME="$(basename "$FILE" .cpp)"
    "$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -MD -EHs-c- -D_HAS_EXCEPTIONS=0 -GR- -GS- -W0 \
        -std:c++17 -DNDEBUG -D_CRT_SECURE_NO_WARNINGS -DASTCENC_NEON=0 -DASTCENC_SVE=0 -DASTCENC_SSE=41 \
        -DASTCENC_AVX=2 -DASTCENC_POPCNT=1 -DASTCENC_F16C=1 -DASTCENC_X86_GATHERS=0 -mavx2 -mpopcnt -mf16c -mfma \
        "$(cygpath -m "$FILE")" -Fo"$(cygpath -m "$WORK/$NAME.obj")"
    OBJECTS="$OBJECTS $(cygpath -m "$WORK/$NAME.obj")"
done
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/Astcenc.lib")" $OBJECTS
cp "$SOURCE/astcenc.h" "$HERE/Include/"
cp "$1/LICENSE.txt" "$HERE/LICENSE"
rm -rf "$WORK"
