# deko3d in SFA-NX

deko3d 0.5.0, https://github.com/devkitPro/deko3d at `5cc144d` (tag v0.5.0, the version of devkitPro's
`libdeko3d.a` SFA-NX linked before; LICENSE: zlib). The copy comes from SwitchWakerHD
(`runtime/third_party/deko3d`), which carries patch 1. `CMakeLists.txt` builds it, with no Makefile, as two
static libraries: `deko3d_release` and `deko3d_debug`. These are upstream's `libdeko3d` and `libdeko3dd`,
built with the same `-DNDEBUG` / `-DDEBUG` and `-O2 -fno-rtti -fno-exceptions -std=gnu++17`.
`switch/cmake/Deko3D.cmake` links one of them in place of devkitPro's archive. Only `include/`, `source/`,
`LICENSE` and `README.md` are kept from upstream. `include/deko3d.h` is identical to devkitPro's.

`generated/` holds the files that upstream's Makefile writes with devkitPro's dekotools
(https://github.com/devkitPro/dekotools, `c15523c`). devkitA64 does not ship dekotools:

    for d in source/maxwell/*.def; do n=$(basename $d .def)
        if [[ $n == *_3d ]]; then dekodef -h generated/$n.h -m generated/$n.mme $d
        else dekodef -h generated/$n.h $d; fi; done
    dekomme -o generated/mme_macros.h generated/engine_3d.mme $(ls source/maxwell/*.mme | sort)

Rerun it after editing a `.def` or `.mme` file.

Each change to upstream files is marked `SwitchWakerHD patch N` or `SFA-NX patch N` in the source.

1. **Separate alpha destination factor** (`source/maxwell/gpu_3d_state.cpp`, `dkCmdBufBindBlendStates`).
   Upstream writes `dstColorBlendFactor` into `FuncAlphaDst`, so a blend state with its own alpha factors
   blends alpha with the colour's destination factor. This is upstream issue #27, still open at `373bfd4`
   (2026-10). Before this fix, SFA-NX drew each such draw twice (RGB, then alpha); the fix removes that
   workaround from `switch/deko/backend_commands.cpp`.
2. **Control-memory chunk leak** (`source/dk_cmdbuf.cpp`, `CmdBuf::clear`). `appendCtrlCmd` pushes every new
   ~1 KiB chunk onto the front of `m_ctrlChunkCur`, but `clear` moved only the head chunk to the free list
   and lost the rest of the chain. SFA-NX clears its command buffer every frame and leaked ~15 MiB a minute,
   until deko3d's allocator failed after ~85 minutes of play (error 2359-0003, `DkResult_OutOfMemory`). The
   fix splices the whole chain. It replaces build 2240's `--wrap=dkCmdBufClear` workaround in
   `switch/deko/memory.cpp`.
3. **No chunk handling on flush-function command buffers** (`source/dk_cmdbuf.cpp`, `CmdBuf::clear`). A
   queue's internal command buffer stores its flush function in the union that holds the chunk pointers.
   `clear` now leaves those pointers alone in that mode, as the destructor already did. No caller clears
   such a buffer today; the guard keeps it that way.

Checked for SFA-NX, no change needed:

- Upstream `bcc3efb`: `dkCmdBufPushConstants` has a 0x7FFC-byte limit, which only the debug build checks
  (`dkCmdBufPushData` too). SFA-NX pushes at most 64 bytes.
- Upstream issue #10: textures 8 rows tall sample the wrong tile. `swdk::image_tile_size_fix`
  (`switch/deko/memory.cpp`) already sets a matching tile size.
- GPFIFO entries coalesce, and one entry holds at most 8 MiB of commands. SFA-NX's per-frame command
  slice is 4 MiB.
