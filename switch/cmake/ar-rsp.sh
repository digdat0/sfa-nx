#!/bin/sh
# ar wrapper for devkitPro's msys2: a Windows program's command line is limited to 32 KB, and Dawn's
# monolithic archive lists thousands of objects. Write the arguments to a response file (Windows
# paths, since msys does not convert paths inside it) and call gcc-ar with @file.
rsp="$(mktemp)"
for a in "$@"; do
  case "$a" in
    /*) printf '%s\n' "$(cygpath -m "$a")" ;;
    *)  printf '%s\n' "$a" ;;
  esac
done > "$rsp"
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc-ar "@$(cygpath -m "$rsp")"
rc=$?
rm -f "$rsp"
exit $rc
