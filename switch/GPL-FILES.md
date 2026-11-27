# GPL-3.0 files

SFA-NX and Foxhollow are CC0 1.0 (see [../LICENSE](../LICENSE)). The files below are the exception: they
come from, or contain code from, [Melee-NX](https://github.com/KawaiiBunga/Melee-NX) by KawaiiBunga,
whose Switch layer is GPL-3.0-or-later. They keep that license ([../LICENSE-GPL-3.0](../LICENSE-GPL-3.0)),
including the SFA-NX changes made to them.

Because the Switch build links these files, the released `SFA-NX.nro` as a whole is distributed under the
GPL-3.0: anyone sharing a modified build must also share its source.

Also GPL-3.0-or-later: the Melee-NX changes inside the deko3d renderer (`switch/deko`, described in
[deko/THIRD_PARTY.md](deko/THIRD_PARTY.md)). The rest of those renderer files keep their MIT and MPL-2.0
notices. `switch/uam` keeps its own licenses (fincs' zlib-style license and Mesa's MIT).

- `switch/CMakeLists.txt`
- `switch/builder/integrate-deko.py`
- `switch/builder/patch-common.sh`
- `switch/builder/prepare-deko.py`
- `switch/builder/seed-caches.py`
- `switch/cmake/DawnOptionsDeko.cmake`
- `switch/cmake/Deko3D.cmake`
- `switch/cmake/Graphics.cmake`
- `switch/cmake/SwitchGCC.cmake`
- `switch/deko/host/deko_host_switch.cpp`
- `switch/deko/host/gpu_submit_diagnostics.cpp`
- `switch/deko/host/posix_stubs_switch.c`
- `switch/patches/aurora-switch-blob-cache-batch.patch`
- `switch/patches/aurora-switch-cache-recovery.patch`
- `switch/patches/aurora-switch-dawn-backends.patch`
- `switch/patches/aurora-switch-encoder-state-cache.patch`
- `switch/patches/aurora-switch-gx-cpu.patch`
- `switch/patches/aurora-switch-io-atomic.patch`
- `switch/patches/aurora-switch-mem1-window.patch`
- `switch/patches/aurora-switch-no-backtrace.patch`
- `switch/patches/aurora-switch-no-mmap.patch`
- `switch/patches/aurora-switch-pad-trigger-bind.patch`
- `switch/patches/aurora-switch-perf-imgui.patch`
- `switch/patches/aurora-switch-platform-compat.patch`
- `switch/patches/aurora-switch-runtime-fixes.patch`
- `switch/patches/aurora-switch-surface.patch`
- `switch/patches/aurora-switch-texture-telemetry.patch`
- `switch/patches/aurora-switch-thread-sweep.patch`
- `switch/patches/aurora-switch-uber-shader.patch`
- `switch/patches/dawn-abseil-switch.patch`
- `switch/patches/dawn-switch-libnx.patch`
- `switch/patches/dawn-switch-renderdoc.patch`
- `switch/patches/dawn-switch-surface.patch`
- `switch/patches/melee-switch-disc-ptr-window.patch`
- `switch/patches/melee-switch-gcc-compat.patch`
- `switch/patches/melee-switch-input-worker.patch`
- `switch/patches/melee-switch-perf-telemetry.patch`
- `switch/patches/melee-switch-rollback-hooks.patch`
- `switch/patches/melee-switch-slippi-hooks.patch`
- `switch/patches/sdl-dusklight-melee-nx.patch`
- `switch/patches/sdl-dusklight-switch.patch`
- `switch/patches/sdl-switch-external-graphics.patch`
- `switch/patches/tracy-switch-platform.patch`
- `switch/src/blob_read_cache.hpp`
- `switch/src/clang_tls_switch.c`
- `switch/src/gx_pipeline_reuse.hpp`
- `switch/src/input_switch.cpp`
- `switch/src/libc_switch.c`
- `switch/src/log_switch.cpp`
- `switch/src/log_switch.h`
- `switch/src/main_sfa.cpp`
- `switch/src/malloc_switch.c`
- `switch/src/nod/gc_disc.c`
- `switch/src/nod/gc_disc.h`
- `switch/src/nod/nod.h`
- `switch/src/nod/nod_shim.c`
- `switch/src/perf_switch.cpp`
- `switch/src/perf_switch.h`
- `switch/src/pipeline_request_state.hpp`
- `switch/src/prof_switch.cpp`
- `switch/src/sqlite_vfs.cpp`
- `switch/src/switch_io_lock.h`
- `switch/src/uber_layout.hpp`
- `switch/src/uber_wgsl.hpp`
