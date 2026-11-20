#!/bin/sh
# Compiler launcher for the decompiled game code (CMakeLists.txt, SFA_NX_GAME_CLANG): CMake calls
# "clang-game.sh <devkitA64 gcc> <args>". Run Clang instead, as Foxhollow upstream does (desktop and Mac
# arm64 builds work there; GCC kept turning decomp UB into spiky geometry). Objects link with
# devkitA64's ld like the rest of the .nro.
shift
CLANG="${SFA_NX_CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
DKA64=/opt/devkitpro/devkitA64
set -- "$@" END
for a in "$@"; do
  shift
  case "$a" in
    END) break ;;
    -mtp=soft) ;;  # devkitPro GCC option (soft TLS); the game code has no thread-locals
    *) set -- "$@" "$a" ;;
  esac
done
exec "$CLANG" --target=aarch64-none-elf -mcpu=cortex-a57 \
  -isystem "$DKA64/aarch64-none-elf/include" \
  -Wno-error -Wno-everything \
  "$@"
