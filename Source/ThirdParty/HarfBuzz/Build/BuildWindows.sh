#!/bin/sh
# Rebuilds Lib/Windows/X64/HarfBuzz.lib from the HarfBuzz release named in HarfBuzz.module.py with the LLVM toolchain,
# from Git Bash.
# Usage: BuildWindows.sh <path to an extracted harfbuzz-<version> source tree>
# The amalgamated src/harfbuzz.cc: the OpenType shaper, the hb-ot font funcs, hb-draw and hb-paint, with no platform
# backends and no subsetter. Allocation goes to MarlinHarfBuzzMalloc/Calloc/Realloc/Free, which the engine defines.
# Against the release DLL runtime every engine configuration uses, without exceptions or RTTI.
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*"
SOURCE="$1/src"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
LLVM="/c/Program Files/LLVM/bin"
"$LLVM/clang-cl.exe" --target=x86_64-pc-windows-msvc -c -O2 -MD -std:c++17 -EHs-c- -D_HAS_EXCEPTIONS=0 -GR- -GS- -W0 \
    -DNDEBUG -Dhb_malloc_impl=MarlinHarfBuzzMalloc -Dhb_calloc_impl=MarlinHarfBuzzCalloc \
    -Dhb_realloc_impl=MarlinHarfBuzzRealloc -Dhb_free_impl=MarlinHarfBuzzFree \
    "$(cygpath -m "$SOURCE/harfbuzz.cc")" -Fo"$(cygpath -m "$WORK/harfbuzz.obj")"
"$LLVM/llvm-lib.exe" -out:"$(cygpath -m "$HERE/Lib/Windows/X64/HarfBuzz.lib")" "$(cygpath -m "$WORK/harfbuzz.obj")"
for HEADER in hb.h hb-blob.h hb-buffer.h hb-common.h hb-deprecated.h hb-draw.h hb-face.h hb-font.h hb-map.h hb-paint.h \
    hb-script-list.h hb-set.h hb-shape.h hb-shape-plan.h hb-style.h hb-unicode.h hb-version.h hb-ot.h hb-ot-color.h \
    hb-ot-deprecated.h hb-ot-fetch.h hb-ot-font.h hb-ot-layout.h hb-ot-math.h hb-ot-meta.h hb-ot-metrics.h hb-ot-name.h \
    hb-ot-shape.h hb-ot-var.h; do
    cp "$SOURCE/$HEADER" "$HERE/Include/"
done
cp "$1/COPYING" "$HERE/LICENSE"
cp "$SOURCE/ms-use/COPYING" "$HERE/LICENSE-ms-use"
rm -rf "$WORK"
