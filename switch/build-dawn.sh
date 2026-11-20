#!/bin/bash
# Build Dawn (Null backend + Tint GLSL writer, WGSL reader/inspector) for Switch with devkitA64 GCC.
#   switch/build-dawn.sh [jobs]
# Needs ref/dawn at encounter/dawn 80ee0043 with switch/patches/dawn-* applied and
# switch/builder/prepare-deko.py run (see switch/README.md). Output: build-switch-dawn/.
set -e
cd "$(dirname "$0")/.."
JOBS="${1:-8}"
# devkitPro install folder as Git Bash sees it (installer default C:\devkitPro); override with SFA_NX_DEVKITPRO.
DKP="${SFA_NX_DEVKITPRO:-/c/devkitPro}"
mkdir -p build-switch-dawn/tmp
T="$(cygpath -m "$PWD/build-switch-dawn/tmp")"
# devkitPro's cmake must run from its own msys2; env vars don't cross from Git Bash into it,
# and gcc needs a Windows temp dir.
RUN=("$DKP/msys2/usr/bin/env.exe"
     "PATH=/opt/devkitpro/msys2/usr/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/bin"
     DEVKITPRO=/opt/devkitpro TMPDIR="$T" TMP="$T" TEMP="$T")
"${RUN[@]}" cmake -S ref/dawn -B build-switch-dawn -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -u "$PWD")/switch/cmake/SwitchFoxhollow.cmake" \
    -C switch/cmake/DawnOptionsDeko.cmake \
    -DPython3_EXECUTABLE=/usr/bin/python3 \
    -UCMAKE_NINJA_FORCE_RESPONSE_FILE
"${RUN[@]}" cmake --build build-switch-dawn --target webgpu_dawn tint_lang_glsl_writer \
    tint_lang_wgsl_reader tint_lang_wgsl_inspector --parallel "$JOBS"
