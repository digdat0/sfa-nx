# SFA-NX

Star Fox Adventures for Nintendo Switch homebrew: a native `.nro` port of
[Foxhollow](https://github.com/JackPriceBurns/foxhollow), the PC port built on the
[SFA decompilation](https://github.com/zcanann/SFA-Decomp).

SFA-NX does not include any Nintendo assets. You need your own legally obtained copy of Star Fox Adventures for
GameCube (GSAE01, GSAP01 or GSAJ01), dumped to an `.iso` file or compressed to an `.rvz` file with Dolphin.
We do not condone piracy and will not respond to requests for game image files!

Note: This is an AI assisted project.

## Getting started

1. You need a Switch running custom firmware (Atmosphère) and the Homebrew Menu.
2. Download `SFA-NX.nro` from the [latest release](https://github.com/digdat0/sfa-nx/releases/latest).
3. Copy it to `sdmc:/switch/SFA-NX/` on your SD card.
4. Copy your disc image into the same folder. Any file name works, but it must be an `.iso`, `.gcm`
   or `.rvz` (Dolphin's compressed format, about a third of the size). Convert `.ciso` with Dolphin
   first. If both an `.iso` and an `.rvz` are there, the `.iso` is used.
5. Start SFA-NX from the Homebrew Menu, opened by holding **R** while launching a game (title takeover).
   Applet mode (from the Album) does not have enough memory.

The first start takes longer while shaders are prepared. Save from the game's own menu.

### Precompiled shaders

SFA-NX ships with about 900 precompiled shaders built into the `.nro`, collected from test play on the Switch and
in Ryujinx. When the game meets a shader that isn't in that set, it compiles it on the spot, which can cause a
short stutter the first time you enter an area or see an effect. Each new shader is saved in the `cache/`
folder, so it only happens once.

You can help everyone get a smoother first play: after a long session or reaching new areas, share your cache.

1. Copy the `cache` folder from `sdmc:/switch/SFA-NX/` to your computer and zip it. Leave out any `.nro`
   files in it (`update.nro`, `previous.nro`): they are only update downloads.
2. Open a [new issue](https://github.com/digdat0/sfa-nx/issues/new) titled "Shader cache", say roughly how far
   into the game you are, and attach the zip.

New shaders from shared caches are merged into the next release. The cache holds no saves or personal data.

SFA-NX creates its own folders next to the `.nro` on the first start:

```
sdmc:/switch/SFA-NX/
├── SFA-NX.nro
├── Star Fox Adventures.iso   (your disc image, or Star Fox Adventures.rvz)
├── config/                   settings
├── cache/                    shaders
├── user/                     saves
├── logs/                     log files
└── mods/
    └── hd/                   HD texture pack (optional, see below)
```

## Options menu

Press **−** in game to open the options menu.

| Option | What it does |
|---|---|
| **Show FPS** | Shows the frame rate in the corner of the screen. |
| **Invert Look** | Flips the vertical stick for the camera, aiming, the Arwing and the CloudRunner flights. |
| **HD Textures** | Turns the HD texture pack on or off (see [HD textures](#hd-textures)). Applies at the next start. |
| **Docked Resolution** | Render size on a TV: 720p, 900p or 1080p. Applies at once. Handheld always renders at 720p. |
| **Widescreen HUD** | **Corrected** (default) keeps the HUD, icons and menus at their original shape in 16:9; **Stretched** shows them stretched to the screen's width, as the original game does. |
| **Language** | The game's language (see [Languages](#languages)). Applies at the next start. |
| **Button Mapping** | Remaps the controls. **Reset To Defaults** restores the GameCube layout. |
| **Updates** | Checks GitHub for a newer release (see [Updates](#updates)). |
| **Extra Logging** | Writes more detail to the log for bug reports (see [Reporting a problem](#reporting-a-problem)). |

Settings are saved at once. When a change needs a restart, a **Restart** row appears at the bottom of the menu.

Default controls follow the GameCube layout: ZL / ZR are the L / R triggers and R is Z. You can change them
under **Button Mapping**.

## Languages

The PAL disc (GSAP01) contains English, French, German, Italian and Spanish. The game starts in English; to
change the language, open **−** → **Language**, pick one with **Left** / **Right**, then choose **Restart**. The
options menu switches to the new language after the restart too. The US (GSAE01) and Japanese (GSAJ01) discs only contain
their own language, so the option shows "PAL disc only" there.

## Reporting a problem

SFA-NX writes `logs/log.txt`, which holds the last 10 times you played. If the game crashes or misbehaves,
turn on **−** → **Extra Logging** (it adds more detail and costs a little speed), play until the problem
happens again, then zip the whole `sdmc:/switch/SFA-NX/logs` folder and attach it to your report. Turn Extra
Logging off again afterwards.

## Updates

Open **−** → **Updates** and press **A** to check GitHub for a newer release. If one is available, press **A**
to download it, then **A** again to restart into the new version. Your saves, settings and mods are kept.
The console needs an internet connection.

## HD textures

SFA-NX supports the community HD texture pack.

1. Download the pack from [GameBanana](https://gamebanana.com/mods/151176).
2. Open the `GSA` folder inside the download and copy everything in it (`Bosses`, `Cape Claw`, ...) into
   `sdmc:/switch/SFA-NX/mods/hd/`.
3. HD textures are on by default. You can turn them off with **−** → **HD Textures**; the change applies at
   the next start.

## Building

See [switch/README.md](switch/README.md). It needs devkitPro (devkitA64, libnx, deko3d) on Windows.
[docs/SWITCH_PORT.md](docs/SWITCH_PORT.md) describes how the port works and how it was optimised for the Switch.

## AI-assisted development

SFA-NX was built with AI assistance (Claude Code): the Switch code, documentation and commit messages were
written with AI and tested on Switch hardware and in Ryujinx.

Foxhollow's author has asked that contributions to Foxhollow be written by humans, without AI-generated
commit messages, pull requests or code comments. We respect that. SFA-NX is an independent project: nothing
from it is submitted to Foxhollow, and please don't send it there or report SFA-NX problems to Foxhollow.

SFA-NX follows its own standards: code and changes are documented thoroughly so the port is easy to follow
and maintain.

## Credits

SFA-NX is built on the work of these projects and people:

- [Foxhollow](https://github.com/JackPriceBurns/foxhollow) by Jack Price-Burns: the native Star Fox Adventures
  port this is based on.
- [SFA-Decomp](https://github.com/zcanann/SFA-Decomp) by zcanann and contributors: the decompilation.
- [Aurora](https://github.com/encounter/aurora) by encounter: the GameCube/Wii platform layer.
- [Dawn](https://dawn.googlesource.com/dawn) and Tint (Google): WebGPU and shader translation.
- [SDL](https://github.com/libsdl-org/SDL): input, audio and windowing.
- [devkitPro](https://devkitpro.org/): devkitA64, libnx, deko3d and the portlibs (curl, zlib).
- [uam](https://github.com/devkitPro/uam) by fincs, with Mesa: the deko3d shader compiler.
- [Melee-NX](https://github.com/KawaiiBunga/Melee-NX) by KawaiiBunga, KartPadNX, and [centollOS/SwitchWaker](https://github.com/centollOS/SwitchWaker): the
  deko3d renderer and Switch platform layer (MIT and MPL-2.0, see [switch/deko/THIRD_PARTY.md](switch/deko/THIRD_PARTY.md)).
- [borealis](https://github.com/encounter/borealis), MusyX and
  [nlohmann/json](https://github.com/nlohmann/json).
- The authors of the [Star Fox Adventures HD texture pack](https://gamebanana.com/mods/151176).
- [Zeedif](https://www.steamgriddb.com/profile/76561198108269802): the app icon, from
  [SteamGridDB](https://www.steamgriddb.com/grid/693481).

## License

SFA-NX is free software under the [GNU General Public License v3.0](LICENSE): you may use, change and share it,
but if you distribute a modified version you must publish its source under the same license and keep the
copyright notices, including credit to SFA-NX (digdat0).

Parts of the repository come from other projects and keep their own licenses, all compatible with the GPL:
Foxhollow and the SFA decompilation are CC0 1.0 ([LICENSE-FOXHOLLOW](LICENSE-FOXHOLLOW)), Aurora is MIT
([extern/aurora/LICENSE](extern/aurora/LICENSE)), and the deko3d renderer code is MIT and MPL-2.0
([switch/deko/THIRD_PARTY.md](switch/deko/THIRD_PARTY.md)).

Star Fox Adventures is © Nintendo and Rare. This project is not affiliated with or endorsed by Nintendo.
