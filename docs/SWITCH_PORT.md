# SFA-NX technical notes

How Star Fox Adventures went from the decompilation to a native Switch homebrew `.nro`, what broke along the way and
what was done to make it run well on the console. Build instructions are in [switch/README.md](../switch/README.md);
this document explains the why.

Timeline: first hardware boot 2026-11-16, playable with HD textures 2026-11-20.

## 1. Starting point

| Layer | Source | Notes |
|---|---|---|
| Game code | [SFA-Decomp](https://github.com/zcanann/SFA-Decomp) | 100% matched decompilation (CC0) |
| PC port | [Foxhollow](https://github.com/JackPriceBurns/foxhollow) | Runs the decomp natively on [Aurora](https://github.com/encounter/aurora) (GX/VI/PAD/DVD/AI on top of Dawn + SDL3) |
| Switch platform | Melee-NX → KartPadNX → centollOS/SwitchWaker | deko3d renderer, uam shader compiler, Dawn/Aurora/SDL patches (MIT + MPL-2.0, [switch/deko/THIRD_PARTY.md](../switch/deko/THIRD_PARTY.md)) |

Foxhollow runs the USA 1.0 code. PAL (GSAP01) and Japanese discs are accepted through Aurora's asset mapping, so
any region's disc works. All Switch work lives in `switch/`; changes to shared code are kept small and behind
`__SWITCH__`.

## 2. Architecture on the console

```
game (decomp, Clang)            Foxhollow port layer         Aurora
  GX calls  ───────────────►  GX FIFO  ─────►  FIFO thread: GX state → pipelines, uniforms, draws
                                                     │
                            direct recorder (switch/deko/direct.cpp)  or  Dawn (Null backend, objects only)
                                                     │
                                              kd shim → deko3d command lists → GPU (render worker)
```

- **Rendering.** Aurora builds WGSL shaders from GX state. Dawn's Tint converts them to GLSL, and `switch/uam`
  (Mesa's GLSL front end, as used by deko3d) compiles them to DKSH. Dawn runs with its Null backend: it only owns
  objects. Command recording goes straight into deko3d through a small WebGPU-shaped shim (`kd`,
  `switch/deko/backend_*.cpp`).
- **Direct renderer.** `switch/deko/direct.cpp` records eligible render passes (viewport, scissor, GX draws,
  clears) straight into deko3d command lists and skips Dawn's render pass and encoder. It is the default
  (− menu → Debug → Renderer).
- **Threads.** The game thread (main), Aurora's FIFO processor, the render worker, the pipeline compiler, the audio
  mixer, the movie decoder and the log writer each get an assigned core and priority (`switch/src/perf_switch.cpp`,
  − menu → Threads).
- **Toolchain.** devkitA64 GCC through devkitPro's msys2 on Windows, no Docker. The game code alone is compiled
  with Clang (section 3.3).

## 3. Getting it to boot and render correctly

### 3.1 Build system

- devkitPro refuses a native Windows CMake, so the build runs devkitPro's msys2 CMake through `env.exe` with
  Windows-style `TMPDIR`/`TMP`/`TEMP`. Otherwise GCC tries to write temporary files to `C:\Windows`.
- msys rewrites any define value that starts with `/` into a `C:/devkitPro/msys2/...` path. `COS_SWITCH_NRO_DIR`
  was silently broken by this, and the SD shader seed was never read. Paths are passed as full `sdmc:/...` strings
  instead (`COS_SWITCH_DATA_DIR`).
- Thousands of objects overflow the command line. The archives go through response files
  (`switch/cmake/ar-rsp.sh`).
- `git apply` run inside the foxhollow repo silently skipped every Aurora patch, because the paths resolved against
  the repo root. `build.sh` now stages Aurora into `build-switch/aurora-deko` with `patch -p1`. The staging stamp
  hashes the working diff, not just committed changes, so uncommitted Aurora edits are picked up.
- `build.sh` fails the build if a game symbol replaces a libnx or SDL symbol. The game's own `padUpdate` replaced
  libnx's this way and broke input on the first hardware run.

### 3.2 Horizon differences

- **Renames.** Horizon's rename cannot overwrite an existing file, so every save failed with "card damaged".
  Fixed by `aurora-switch-io-atomic.patch` and `aurora-switch-card.patch`.
- **No mmap, signals or dlopen.** Patched out (`aurora-switch-no-mmap.patch`, `aurora-switch-no-backtrace.patch`).
  SQLite gets its own Horizon VFS (`switch/src/sqlite_vfs.cpp`).
- **Launch mode.** The app checks for title takeover at start and shows a message in applet mode (Album), which
  has too little memory. It also checks for a disc image.

### 3.3 Rendering glitches from the compiler

The first builds drew fire as huge starbursts and models as spiky geometry. Three causes were found in turn:

1. **Float-to-integer conversion.** On AArch64, GCC converts float to u8/u16 with `fcvtzu`, which clamps: negative
   values become 0 and overflow sticks at the maximum. The GameCube's `fctiwz` and Clang go through s32 and wrap.
   Particle scale overshoots therefore stayed huge instead of wrapping. About 220 sites were patched with `(s32)`
   casts, found with `-Wfloat-conversion` and `-fsanitize=float-cast-overflow`.
2. **Reads past the end of globals.** The decomp reads past one global into the next one: the expgfx quad
   templates read into `gObjFxCrystalAmpTbl`. GCC's data sections and reordering break that layout.
3. **Uninitialised locals** that happen to work with Clang's code generation.

The final fix was to compile the game code with Clang 23 (`switch/cmake/clang-game.sh`, option
`SFA_NX_GAME_CLANG`), which matches Foxhollow's upstream compiler. The rest of the build stays on GCC. Hardware
confirmed that all rendering issues were gone (build 1404).

### 3.4 GPU hang (2348-0006 / 2345-0002)

- **GPFIFO overflow.** In deko3d, each `DkBarrier_Full` and each flushing fence signal ends two GPFIFO entries,
  and entries are only kicked off at a queue flush. A map load built one list of 1479 entries, which libnx refused.
  `swdk::gpfifo_entries_recorded()` now submits and flushes every 128 estimated entries.
- **Breadcrumbs.** Every non-draw GPU operation writes a marker. If a frame fence hasn't signalled after 2 s, the
  markers are dumped (`[gpu-breadcrumb]`), so a hang on hardware can be traced to an operation.

### 3.5 Input

- SDL's Switch mapping reported the physical ZL as L and L as ZL.
  `switch/patches/sdl-dusklight-sfa-nx-shoulders.patch` fixes it. The defaults follow the GameCube layout:
  ZL/ZR = L/R, R = Z.
- Button remapping and invert look are in the − menu. Invert look covers the viewfinder, the Arwing, staff aiming
  and the CloudRunner sections.

## 4. Performance work

The target is a steady 60 fps at 720p in handheld mode. Lowering the resolution was ruled out. Every change was
measured with the built-in sampling profiler (− menu → Debug → Profiler, reports in `logs/profile-NNN.txt`,
summarised by `switch/tools/prof_report.py`) and checked on hardware.

### 4.1 Threads and locks

| Problem (found in profiles) | Fix |
|---|---|
| The FIFO processor and render worker shared core 0 with the game; Aurora's thread pinning only worked on Linux | `melee_nx_perf_thread` assigns cores; the game thread gets core 1 to itself |
| Pipeline compiler and cache writer at the FIFO thread's priority on its core (equal priority = no preemption on Horizon) | Lower priority, cores 0\|2 |
| A libstdc++ mutex + condvar notify on every draw (3.3 ms/frame in `svcArbitrateLock`) | Publish every 8 draws, wake only sleeping waiters |
| Main thread waited for the FIFO thread at frame end | Frame overlap: double command buffers; the FIFO thread submits the previous frame while the game runs the next |
| The FIFO thread processed while holding the buffer mutex | Processing moved outside the lock |
| Texture/sampler AddRef/Release took the global API lock on every draw | Lock-free reference counts |
| Queue submit replayed a whole frame under the object lock and blocked resource creation | Separate queue lock, fixed lock order checked at runtime |
| Depth readback blocked the main thread for 3 ms/frame | Depth snapshots on their own mutex |

The profiler attributes lock waits to a mutex address and caller. That is how most of these were found.

### 4.2 GPU

All of these are − menu options. The user's verdict on hardware was to keep them all on.

- **GPU clock.** The official handheld profile `0x92220008` (GPU 460.8 MHz instead of the default 307.2). It is a
  retail profile, not an overclock, and the original configuration is restored at exit.
- **Render compression** for colour and depth. Copies from compressed targets are done by draws (`copy_by_draw`),
  because the plain image copy stalled for about 2 ms.
- **Z-cull.**
- **BC1 textures.** Opaque GameCube CMPR textures are uploaded as BC1 instead of being decoded to RGBA8. The worst
  channel difference is 10.7/255.
- **Always on:**
  - The resample pass is skipped when the source size equals the viewport.
  - Barriers between passes are only placed where they are needed.

Result on hardware: the frame fence wait dropped from 3–7 ms to about 0 ms, so the GPU is no longer the limit.

### 4.3 Shaders and loading

- **Generic vertex shader.** Texgen and lighting are read from the uniform buffer instead of being specialised per
  pipeline. Vertex and fragment shaders became separate modules, so one vertex shader serves many fragment
  shaders. Over all 1340 known recipes this went from 567 to 393 shaders, and total compile time dropped by 20%
  (23.3 s to 18.7 s).
- **Shipped shader seed.** Compiling a pipeline on the console (WGSL → Tint → GLSL → uam) takes 170–260 ms. After a
  cache wipe, the first loads showed a black screen for about two minutes. `switch/builder/seed-caches.py` merges
  console and Ryujinx caches into `switch/seed/initial_dksh_cache.bin` (989 compiled shaders) and
  `initial_pipeline_cache.db` (3130 pipeline recipes). Both are packed into the NRO's RomFS and read from
  `romfs:` at start. In Ryujinx, a pipeline build dropped from 313 ms to 1.3 ms with the seed.
- **Uniforms.** Rebuilt only when the pipeline configuration changed, and written straight into the frame's
  uniform buffer.

### 4.4 Hitches

After the throughput work, the average frame fit in budget but 7–15% of frames still took over 20 ms. Each cause
was found with the profiler's per-slow-frame records:

| Hitch | Fix |
|---|---|
| stdout written to the SD card synchronously (1–2 ms per frame, occasional 150–350 ms stalls) | Asynchronous log writer thread (`sfalog:` devoptab) |
| The profiler writing a 1.5 MB report on the main thread (a 3–7 s freeze every 2 minutes) | Snapshot on main, written by a background thread |
| The audio IRQ waited up to 92 ms because the frame wait held the interrupt lock | The lock is released for the whole of `VIWaitForRetrace` |
| Timer jitter around 1.0 frame produced periodic double game steps (judder) | Whole-frame steps: the game delta is rounded to 60ths and the remainder carried over |
| Pacing on the SDL timer | Pacing on the display's vsync event; libnx's default window is replaced so its display can be reused (a second `viOpenDefaultDisplay` fails on hardware) |
| SDL's Switch audio `PlayDevice` blocked under the device lock | Queue and return (`sdl-dusklight-sfa-nx.patch`) |
| THP movie decode (about 10 ms) ran in a VI callback under the interrupt lock | Movie decode on its own thread (`port/src/movie_worker.c`) |
| MusyX mixing on the game thread (about 1.4 ms per frame) | Its own audio thread, with an adaptive output queue (40–120 ms) |

Hardware result (build 2158): about 31 slow frames in 7181 during gameplay (0.4%), at 58–60 presents per second.

### 4.5 Measured gains

Build numbers are the build time (HHMM), so they are listed with the date. Unless marked Ryujinx, every number comes
from a hardware profile taken in handheld mode at 720p.

**Frame rate over time**

| Build (date) | Change | Average frame | Frames over 20 ms | Notes |
|---|---|---|---|---|
| 1814 (11-18) | Starting point, before the thread work | about 22 ms (about 45 fps) | — | Game thread busy only 9 ms; 13 ms waiting on the FIFO processor and render worker, which shared its core |
| 2046 (11-18) | Thread cores assigned, MusyX on its own thread | 21.6 ms | — | Game 7.5 + present 9.3 ms: the main thread waited for the FIFO thread, which lost 3.3 ms/frame to per-draw lock notifies |
| 2126 (11-18) | Batched notifies, frame overlap | 21 ms | — | Main thread now spent 7 ms/frame waiting on mutexes; this led to the lock-wait profiler |
| 2204 (11-18) | FIFO processing outside its lock, lock-free texture refs | 19.3 ms (about 52 fps) | 15% | GPU frame fence 1–6 ms |
| 0757 (11-19) | Direct renderer (bypasses Dawn's render pass) | — | — | Render worker malloc/free 2.5 → 0.9 ms/frame; felt smoother, with fewer audio dropouts |
| 1758 (11-19) | GPU clock, compression, Z-cull, BC1 | heavy scenes 17.9 → 16.5 ms | — | GPU pass time −2.5 to −3 ms, partly lost to a new 2 ms copy stall from compressed targets |
| 1941 (11-19) | Draw-based copies from compressed targets | 18.3 ms | 7.7% | Frame fence wait 3–7 ms → about 0: the GPU stopped being the limit, so the remaining drops were hitches |
| 2014 (11-19) | Asynchronous log, interrupt lock released during the frame wait, whole-frame steps | — | 0.2–0.8% in gameplay | Game 8.4–9.4 ms per frame |
| 2158 (11-19) | Vsync pacing, profiler written off the main thread, SDL audio fix | — | 0.4% (31 of 7181) | 58–60 presents per second |

So the average frame went from about 22 ms to the 16.7 ms budget. In gameplay, frames over 20 ms dropped from 15%
of frames to under 1%.

**Individual measurements**

| What | Before | After |
|---|---|---|
| Lock overhead on the FIFO thread (`svcArbitrateLock`) | 3.3 ms/frame | Batched; no longer near the top of the profile |
| Main thread waiting on depth readback | 3 ms/frame | Own mutex; no longer seen waiting |
| Present on the main thread (Ryujinx) | 7.8 ms | 2.3 ms |
| stdout written to the SD card on the game thread | 1.25 ms/frame (+0.93 ms on the render worker) | Background writer |
| Profiler report write | 6.8 s freeze on the main thread every 2 min | 274–543 ms on a background thread |
| Longest audio interrupt wait | 63–92 ms (crackle) | Lock released for the frame wait; THP movie decode moved off the interrupt lock |
| GPU frame fence wait | 3–7 ms (7–11 ms in cutscenes) | About 0 ms |
| Shader compile time for all 1340 recipes (host uam) | 23.3 s (567 shaders) | 18.7 s (393 shaders), −20% |
| Pipeline build on first use (Ryujinx) | 313 ms | 1.3 ms with the shipped seed |
| Black-screen loading after a cache wipe | About 2 minutes | Seeded shaders load from RomFS; only new shaders are compiled |

## 5. Features added for the Switch

- **− options menu** (`switch/src/switch_menu.cpp`, ImGui):
  - **Game tab:** FPS, invert look, HD textures, button mapping, updates.
  - **Debug tab:** renderer, threads, GPU options, pacing, profiler, shader tour.
  - Settings are saved in `config/switch_menu.ini`.
- **HD textures** (`switch/src/hd_textures_switch.cpp`). The GameBanana pack goes in `mods/hd/`; Aurora's texture
  replacement loads DDS files, with an LRU cache capped at 512 MB. 34 files in the pack that don't match the
  curated set are skipped by hash.
- **In-game updater** (`switch/src/updater_switch.cpp`):
  - Checks GitHub releases (libcurl with libnx SSL) and downloads `SFA-NX.nro` to `cache/update.nro`.
  - Verifies size, NRO magic and the release's SHA-256 digest.
  - Swaps the file in, keeping the previous one as `cache/previous.nro` until the next start, and relaunches with
    `envSetNextLoad`.
- **Shader tour** (`switch/src/shader_tour_switch.c`): warps through every map to fill the shader caches, used to
  build the seed. Left/Right on its menu line picks a route. Route 1 stops where each warp lands. Route 2 then
  teleports to the loaded object farthest from that point, and route 3 makes one more hop to the object farthest
  from both, so each route reaches parts of the maps the others miss. Some warps land on a warp pad that sends
  the player straight on (warps 93 and 94 ping-pong between each other). A stop therefore counts only after the
  player has stayed loaded for 3 s in a row, and a warp whose player vanishes 3 times is written to
  `config/shader_tour_skip` and skipped by every later tour. Delete that file to try those warps again.
- **SD layout:** only the NRO, the disc image and folders (`config/`, `cache/`, `user/`, `logs/`, `mods/`). Logs and
  crash reports from Atmosphere are collected in `logs/`.

## 6. Testing

- **Hardware.** Every build's ELF is kept in `debug/SFA-NX-HHMM.elf` so crash addresses can be symbolised against
  the exact build. Logs come from `sdmc:/switch/SFA-NX/logs/`.
- **Ryujinx.** Used for smoke tests: the attract demo loads maps with no input. Ryujinx can't show GPU clock,
  compression or Z-cull effects, its GPU timestamps read 0, and its uam times are distorted. Performance numbers
  are only taken on hardware.
- **Offline shader timing.** Captured GLSL is compiled with the host `uam.exe` to compare shader variants.

## 7. Open items

- The updater's relaunch has not been tested on hardware (Ryujinx does not implement `envSetNextLoad`).
- The cause of the original GPU hang (2348-0006) has not been confirmed. The breadcrumbs are in place for the next
  occurrence.
- Native `mod.dll` parts of PC mods (beta/health/logo variants, glyphs) do not run on the Switch.
