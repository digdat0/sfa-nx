# SFA-NX: Foxhollow on Nintendo Switch

Star Fox Adventures as a Switch homebrew `.nro`, built from Foxhollow (the SFA decomp on Aurora).
Rendering: Aurora GX → Dawn (Null backend, objects only) → `switch/deko` replays each frame on
deko3d; Tint turns WGSL into GLSL and `switch/uam` compiles it to DKSH. The renderer, `switch/uam`,
`switch/src` and the Aurora/Dawn/SDL patches come from Melee-NX (KawaiiBunga), which took them from
KartPadNX and centollOS/SwitchWaker: MIT and MPL-2.0, see `switch/deko/THIRD_PARTY.md` and the
licence files there. Keep those notices when distributing.

Everything is built with devkitA64 GCC on Windows through devkitPro's msys2 (no Docker, no Clang).

## Build (Windows, Git Bash)

Needs devkitPro with `switch-dev switch-tools switch-zlib switch-curl` (deko3d and uam ship with
switch-dev / switch-tools), plus Python 3 as `py`.

Once:

```bash
# sources (ignored, under ref/)
git init ref/dawn && git -C ref/dawn remote add origin https://github.com/encounter/dawn
git -C ref/dawn fetch --depth 1 origin 80ee0043018a51532ea0fa2e77496cc66634157e && git -C ref/dawn checkout FETCH_HEAD
git -C ref/dawn submodule update --init --depth 1 third_party/abseil-cpp third_party/jinja2 third_party/markupsafe third_party/spirv-headers/src
git clone --depth 1 -b release-3.4.10 https://github.com/libsdl-org/SDL ref/SDL-dusklight
# patches
(cd ref/dawn && for p in libnx surface; do git apply --ignore-space-change ../../switch/patches/dawn-switch-$p.patch; done)
(cd ref/dawn/third_party/abseil-cpp && git apply --ignore-space-change ../../../../switch/patches/dawn-abseil-switch.patch)
#   dawn-switch-renderdoc.patch is malformed: add `|| defined(__SWITCH__)` to the __linux__ line of
#   ref/dawn/third_party/renderdoc/renderdoc/api/app/renderdoc_app.h by hand.
py -3 switch/builder/prepare-deko.py --dawn ref/dawn
(cd ref/SDL-dusklight && for p in switch melee-nx sfa-nx sfa-nx-shoulders; do git apply --recount --ignore-space-change ../../switch/patches/sdl-dusklight-$p.patch; done)
# libraries
bash switch/build-dawn.sh      # build-switch-dawn/  (Dawn Null + Tint, ~15 min the first time)
bash switch/build-sdl.sh       # build-switch-sdl/libSDL3.a
```

Then, every time:

```bash
bash switch/build.sh           # -> build-switch/sdcard/switch/SFA-NX/SFA-NX.nro, debug/SFA-NX-HHMM.elf
```

`build.sh` re-stages Aurora (`extern/aurora` + `switch/patches/aurora-switch-*.patch` +
`switch/builder/integrate-deko.py`) into `build-switch/aurora-deko` when those inputs change.

## Install

```text
sdmc:/switch/SFA-NX/SFA-NX.nro
sdmc:/switch/SFA-NX/<your disc>.iso     # your own GSAE01 / GSAJ01 / GSAP01 image (.iso, .gcm or .rvz)
```

Start it with title takeover (hold R while launching a game, then open the Homebrew Menu). The shader
and pipeline seeds (`switch/seed/`) are packed into the NRO's RomFS. Logs are in `logs/` (`boot.txt`,
`log.txt` with the last 10 runs; `extra_logging=1` in `config/switch_menu.ini` or the menu's Extra Logging adds
the leak finder and GPU samples); saves in `user/`, settings in `config/`, shader caches in `cache/`. Not supported on
Switch: native mod DLLs, the desktop crash handler, Wii and WIA images. GameCube RVZ (zstd) is
decoded on the fly by `src/nod/rvz.c`.

How the port works and what was optimised: [docs/SWITCH_PORT.md](../docs/SWITCH_PORT.md).

## Repository history and Foxhollow updates

The SFA-NX repository keeps Foxhollow's history (its authors, dates and messages), with two files removed from
every commit: `extern/aurora/assets/screenshot.png` (a Nintendo screenshot from upstream Aurora) and `CLAUDE.md`.
SFA-NX's own work sits on top.

- `switch/tools/sync-upstream.sh` rebuilds the local branch `upstream-clean` from Foxhollow's `main`. The
  filter is deterministic, so each run only adds Foxhollow's new commits. Then run `git merge upstream-clean`.
- `switch/tools/hooks/pre-push` (copy it to `.git/hooks/pre-push`) refuses a push whose commits are not by
  digdat0 (filtered Foxhollow commits excepted) or contain a Nintendo asset or game file: the removed paths,
  disc images (`.iso`, `.rvz`, ...), executables (`.dol`, `.rel`, `.elf`), saves (`.gci`, `.sav`) or keys.
- Build scripts take the devkitPro folder from `SFA_NX_DEVKITPRO` (default `C:\devkitPro`): no local paths in
  the repository.
