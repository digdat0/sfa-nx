#!/bin/bash
# Build SDL3 (3.4.10 + switch/patches/sdl-dusklight-*.patch) as a static library for Switch.
#   switch/build-sdl.sh [jobs]      Output: build-switch-sdl/libSDL3.a
set -e
cd "$(dirname "$0")/.."
JOBS="${1:-8}"
# devkitPro install folder as Git Bash sees it (installer default C:\devkitPro); override with SFA_NX_DEVKITPRO.
DKP="${SFA_NX_DEVKITPRO:-/c/devkitPro}"
mkdir -p build-switch-sdl/tmp
T="$(cygpath -m "$PWD/build-switch-sdl/tmp")"
RUN=("$DKP/msys2/usr/bin/env.exe"
     "PATH=/opt/devkitpro/msys2/usr/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/bin"
     DEVKITPRO=/opt/devkitpro TMPDIR="$T" TMP="$T" TEMP="$T")
"${RUN[@]}" cmake -S ref/SDL-dusklight -B build-switch-sdl -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_EXAMPLES=OFF -DSDL_SWITCH_EXTERNAL_GRAPHICS=ON \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER_GPU=OFF
"${RUN[@]}" cmake --build build-switch-sdl --target SDL3-static --parallel "$JOBS"
