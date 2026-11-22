#!/bin/bash
# Build SFA-NX.nro (Foxhollow for Switch).   switch/build.sh [jobs]
# Prerequisites (once): switch/build-dawn.sh and switch/build-sdl.sh (see switch/README.md).
# Each run re-stages Aurora (extern/aurora + switch/patches/aurora-switch-*.patch + deko3d seams)
# into build-switch/aurora-deko, then configures and builds build-switch/.
# Output: build-switch/SFA-NX.nro, copied with its .elf to debug/SFA-NX-HHMM.elf.
set -e
cd "$(dirname "$0")/.."
JOBS="${1:-8}"
# devkitPro install folder as Git Bash sees it (installer default C:\devkitPro). Set SFA_NX_DEVKITPRO for
# another location, e.g. SFA_NX_DEVKITPRO=/d/devkitPro switch/build.sh. No local paths are hard-coded here.
DKP="${SFA_NX_DEVKITPRO:-/c/devkitPro}"

# ---- stage Aurora (only when its inputs changed)
STAMP=build-switch/aurora-deko.stamp
mkdir -p build-switch
# Uncommitted edits under extern/aurora count too: with only the committed tree in the stamp, local
# Aurora changes never reached the build (2026-11-17: two test builds were identical to the one before).
AURORA_LOCAL="$( { git diff HEAD -- extern/aurora; git ls-files -o --exclude-standard -- extern/aurora | xargs -r cat; } 2>/dev/null | sha1sum | cut -c1-12)"
INPUTS="$(cat switch/patches/aurora-switch-*.patch switch/builder/integrate-deko.py | sha1sum | cut -c1-12)-$(git rev-parse HEAD:extern/aurora 2>/dev/null || echo tree)-$AURORA_LOCAL"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$INPUTS" ] || [ ! -d build-switch/aurora-deko ]; then
  echo "== staging Aurora"
  rm -rf build-switch/aurora-patched build-switch/aurora-deko
  cp -r extern/aurora build-switch/aurora-patched
  # Applied with patch(1): git apply inside the foxhollow checkout resolves paths against the repo root
  # and silently skipped every patch. The other aurora-switch-*.patch files are Melee-NX's, written for
  # an older Aurora; they were never active here (the game runs without them) and many conflict.
  (cd build-switch/aurora-patched && for p in io-atomic card fcvt; do
    patch -p1 -N -s --no-backup-if-mismatch -i "../../switch/patches/aurora-switch-$p.patch"
  done)
  py -3 -I switch/builder/integrate-deko.py --src build-switch/aurora-patched --dst build-switch/aurora-deko
  echo "$INPUTS" > "$STAMP"
fi

# ---- configure + build through devkitPro's msys2 (env vars don't cross from Git Bash; gcc needs a
# Windows temp dir)
mkdir -p build-switch/tmp
T="$(cygpath -m "$PWD/build-switch/tmp")"
RUN=("$DKP/msys2/usr/bin/env.exe"
     "PATH=/opt/devkitpro/msys2/usr/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/bin"
     DEVKITPRO=/opt/devkitpro TMPDIR="$T" TMP="$T" TEMP="$T")
"${RUN[@]}" cmake -S switch -B build-switch -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -u "$PWD")/switch/cmake/SwitchFoxhollow.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -UCMAKE_NINJA_FORCE_RESPONSE_FILE
touch switch/src/main_sfa.cpp # boot.txt / stdout.log build stamp = this build
"${RUN[@]}" cmake --build build-switch --parallel "$JOBS" ${KEEP_GOING:+-- -k 0}

# The game links whole-archive with --allow-multiple-definition, so a game function named like a
# libnx/SDL one silently replaces it (padUpdate did: crash before the game started). Fail instead.
# The game's own float trig (acosf, asinf, atanf, atan2f) replaces libm's on desktop too: allowed.
NM="$DKP/devkitA64/bin/aarch64-none-elf-nm.exe"
syms() { "$NM" -g --defined-only "$@" 2>/dev/null | awk 'NF==3{print $3}' | sort -u; }
CLASH="$(comm -12 <(syms build-switch/libgame.a build-switch/libport_shims.a) \
                  <(syms "$DKP"/libnx/lib/libnx.a build-switch-sdl/libSDL3.a) \
         | grep -vxE 'acosf|asinf|atanf|atan2f' || true)"
if [ -n "$CLASH" ]; then
  echo "error: game symbols replace libnx/SDL ones (rename them in CMakeLists.txt):"; echo "$CLASH"; exit 1
fi

# The heap is dlmalloc (switch/src/malloc_switch.c). If any newlib malloc object still links in (some
# entry point malloc_switch.c does not define), --allow-multiple-definition would hide the clash and two
# allocators would share one heap: fail instead. __malloc_av_ is newlib's bin array (libc_a-mallocr.o).
if "$NM" build-switch/SFA-NX.elf | grep -qw "__malloc_av_"; then
  echo "error: newlib's malloc is linked next to dlmalloc (switch/src/malloc_switch.c misses an entry point)"
  exit 1
fi

mkdir -p debug build-switch/sdcard/switch/SFA-NX
cp build-switch/SFA-NX.elf "debug/SFA-NX-$(date +%H%M).elf"
cp build-switch/SFA-NX.nro build-switch/sdcard/switch/SFA-NX/
rm -f build-switch/sdcard/switch/SFA-NX/initial_dksh_cache.bin build-switch/sdcard/switch/SFA-NX/initial_pipeline_cache.db
ls -la build-switch/sdcard/switch/SFA-NX/SFA-NX.nro
