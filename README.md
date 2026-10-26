<div align=center>

<img src="app/futari_icon.jpg" alt="Mushihimesama Futari" width="30%">

</div>
<h1 align=center>Mushihimesama Futari — Nintendo Switch port</h1>

A native Nintendo Switch port of the Xbox 360 version of **Mushihimesama Futari Ver 1.5** (CAVE, 2009), with the
**Black Label** and **Ver 1.01** downloadable content.

It's not an emulator. The PowerPC code of the game's `default.xex` and of its eight game cores (`media/Module/*.xex`)
is statically recompiled to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk) and compiled for the Switch, and
the game's Direct3D calls are drawn directly on the Switch GPU's own API, deko3d, by
[reNut](https://github.com/masterspike52/reNut)'s Direct3D-level renderer (version 1.0.1).

> [!NOTE]
> You need your own copy of the Xbox 360 disc (a disc image, `.iso`). The Black Label and Ver 1.01 DLC packages
> (`Content/<profile>/435A07D7/00000002`) are optional.

## How to install

1. Open the installer page: **https://chanseyisthebest.github.io/futari-nx-installer/**
2. Choose your game: its **disc image** (`.iso`; you can choose the DLC package files along with it) or a **folder**
   you extracted the game into. Then press **Create futari-nx.zip**. Everything runs in your browser and the game
   files never leave your computer: the page checks your copy, downloads the build and makes the shaders from your
   game's own executables.
3. Extract the downloaded `futari-nx.zip` into `sdmc:/switch/`.

Your `futari-nx` folder should look like this:

```text
/switch/futari-nx/
  futari.nro
  LICENSES.txt
  futari.toml               (your settings; written at the first start when it is missing)
  deko3d_glsl.bin           (the game's shaders, made from your copy)
  game_root/                (the files of your disc)
  dlc/                      (optional: Black Label and Ver 1.01, installed by the game at its first start)
```

The game adds `deko3d_shaders.bin` and `pipeline_list.bin` there as you play (the shaders compiled for the Switch
GPU, and the list of what it drew, so later sessions prepare it ahead).

Launch it with a game override (hold **R** while starting an installed title) or a forwarder. Album applet mode does
not give the game enough memory. If you use a forwarder, set it to a **39-bit address space**.

To build everything yourself instead, see [docs/building.md](docs/building.md).

> [!NOTE]
> **Updating:** open the installer page again and press **Create futari-nx-update.zip**. It only holds the new
> `futari.nro` and the shaders, so there is no need to copy the game again: extract it over your folder. Your
> `futari.toml`, your saves and the game files stay. Coming from 1.0.0, `futari_shaders.mesp` (+ `.idx`) and the
> `cache` folder are no longer used: you can delete them.

> [!CAUTION]
> **The first time you play** after installing or updating, the game compiles its shaders for the Switch GPU as it
> meets them, so **IT CAN STUTTER for a moment** in new places. They are saved next to the NRO
> (`deko3d_shaders.bin`) and later sessions start smoothly.

## Controls

By default the face buttons work **by position, as on the Xbox 360 pad** the game was made for: the bottom button is
the Xbox's A, the right one is the Xbox's B, and so on.

<div align="center">

| Switch | Xbox 360 |
| --- | --- |
| B (bottom) | A |
| A (right) | B |
| Y (left) | X |
| X (top) | Y |
| ZR / ZL | RT / LT |
| R / L | RB / LB |
| + | Start |
| − | Back |
| Sticks / D-pad | Sticks / D-pad |

</div>

To use the buttons by letter instead (the Switch's A acts as A), set `input_xbox_layout = false` in `futari.toml`.

**Two players:** every controller is its own player, in the order they connect. The console in handheld mode counts
as one, so handheld + a Pro Controller makes players 1 and 2. Single Joy-Cons are held sideways, one per player (SL
and SR are the triggers). **L + R + ZR** opens the system's controller screen to connect, reorder, or split a Joy-Con
pair.

**Remapping:** each player has a button map in `futari.toml` (`[input_p1]` and `[input_p2]`).

## Settings

`futari.toml`, next to the NRO, holds only your options. Everything else is built into the NRO.

<div align="center">

| Setting | What it does |
| --- | --- |
| `input_xbox_layout` | Face buttons by position (`true`, default) or by letter (`false`) |
| `input_joycon_sideways` | Single Joy-Cons held sideways, one per player (`true`, default) |
| `input_joycon_sideways_stick` | With it, a single Joy-Con's stick turned for the sideways grip (`true`, default) |
| `[input_p1]`, `[input_p2]` | Each player's buttons: for every Xbox button, the Switch buttons that press it, e.g. `a = "B, ZR"` |

</div>

If you delete `futari.toml`, the game writes the default one again at the next start.

## Performance

The port runs at the console's stock clocks, at a steady 60 fps. The released NRO is built with link-time
optimisation, PGO from a console session of this version and the hottest functions first in memory. Every
optimisation has its own setting and was checked on the PC to give exactly the same picture, frame for frame: the
measurements are in [docs/futari-notes.md](docs/futari-notes.md).

Version 1.0.1 draws with a new renderer: [reNut](https://github.com/masterspike52/reNut)'s Direct3D-level renderer
(as the Banjo-Kazooie: Nuts & Bolts port, banjo-nx, adapted it) on deko3d, the Switch GPU's own API. The game's
Direct3D calls are drawn directly, without writing and reading back the Xbox 360's GPU command stream and without a
model of its EDRAM; the drawing runs on its own thread. On the console a frame takes about 1-4 ms of that thread and
4-10 ms of the GPU, out of 16.7. It takes Sonic Adventure 2's (sa2-nx's) CPU work for this renderer too: each draw
copies only the part of the Direct3D state that changed, so the game's thread spends far less time per draw. Input lag
is lower: the game reads the controllers as late in its frame as it safely can, its 60 Hz follows the screen's own
refresh, and the screen shows the newest picture with two images instead of three.

It also compiles the game's code better: the improvements of the code generator and the steps after it from
[sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx)'s three CPU rounds (the game's registers kept in the processor's
registers, constants folded, fewer floating-point mode switches and memory barriers, direct calls where the game called
through a pointer), on the game's program and all eight game cores. It also corrects two things 1.0.0 did differently
from the Xbox 360 (a float-to-integer conversion rounded halves away from zero; a compiler flag that allowed
reordering float operations across a mode switch), and guards against the black textures sa2-nx saw after hours of
play (its texture-slot fix). On top of that it has the Banjo-Kazooie: Nuts & Bolts port's (banjo-nx's) codegen
round: shifts and bit-field extracts where the game
rotates words, register writes and write-backs nothing reads left out, constants found across loop heads, single
floats computed in single precision, small memory copies inline, the stack pointer and link register kept where they
are provably unchanged, and copies of tiny functions where they are called. Each step was checked against 1.0.0 on the
PC, including frame by frame over a replay of the game.

## How it works

- **The game's code is translated, not emulated.** [ReXGlue](https://github.com/rexglue/rexglue-sdk) translates every
  function of the game's program and of its eight game cores (one per character and mode, all built for the same
  address and linked statically on the Switch) to C++, which is then compiled for the Switch. ReXGlue also gives the
  game what it expects from an Xbox 360: its system, files, audio and controllers.
- **A Direct3D-level renderer on deko3d** (1.0.1). The game's Direct3D calls are replaced by
  [reNut](https://github.com/masterspike52/reNut)'s renderer (as banjo-nx adapted it), which draws them with
  [plume](https://github.com/renderbag/plume) on [deko3d](https://github.com/devkitPro/deko3d), the Switch GPU's own
  API: no GPU command stream is written or parsed and no EDRAM is modelled.
- **Shaders made from your copy.** Futari keeps all its shaders inside its executables. The installer page takes them
  from your copy and makes `deko3d_glsl.bin` in your browser: Renut's [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)
  translates each one to HLSL, DXC compiles it to SPIR-V, [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)
  turns that into GLSL and banjo-nx's passes trim it (the way peggle2-nx's installer does it, checked against the
  PC tools). The console compiles each one with [uam](https://github.com/devkitPro/uam) the first time
  it is used and keeps the result. The NRO itself holds no shader of the game.
- **1.0.0's renderer (`futari_renderer = "default"`).** The game's GPU command stream drawn with Vulkan on NVK (Mesa,
  through [mesa-switch](https://github.com/danfromtico/mesa-switch), in the build of
  [nfsmw-nx](https://github.com/StevensND/nfsmw-nx)), with the busiest Direct3D paths replaced by native code. It
  stays in the source; the released NRO no longer links the Vulkan driver (`-DFUTARI_SWITCH_NVK=ON` brings it back).
- **Audio.** The game's own mixer output goes to the Switch's audio service, unchanged. When the game replays a sound
  on one of its sound channels it destroys the one still playing there, which cut loud sound effects dead and clicked
  (the crackle of busy Xbox 360 mode play). Since 1.0.3 a sound cut that way fades out over one audio frame (5.3 ms)
  first, at the point where the game cuts it; before, the output smoothed the clicks afterwards.

## Documentation

<div align="center">

| Document | What it explains |
| --- | --- |
| [docs/building.md](docs/building.md) | How to build the PC test build, the NRO, the shader package and the SD folder |
| [docs/futari-notes.md](docs/futari-notes.md) | The port step by step: findings in the game's code, every optimisation and how it was checked |
| [docs/research/](docs/research/) | Research notes and comparisons with the sibling ports |

</div>

## Build

Windows, LLVM clang 20 and the VS 2022 Build Tools for the PC build; devkitA64, libnx and deko3d from
[devkitPro](https://devkitpro.org) for the Switch (NVK from [mesa-switch](https://github.com/danfromtico/mesa-switch)
only for 1.0.0's renderer); Python; and your own copy of the game for the code generator and the shaders. The whole process is in
[docs/building.md](docs/building.md).

<div align="center">

| Folder | Contents |
| --- | --- |
| `app/` | The game: hooks, native renderer, native functions, settings, CMake project |
| `sdk/` | ReXGlue SDK with the Horizon layer and the code generator changes |
| `shaders/` | XenosRecomp with the ports' changes, the shader package tools and the installer's WebAssembly builds |
| `tools/` | Code generation steps, build and test scripts, analysis tools |
| `patches/` | Every change to third-party code (the SDK, FFmpeg, XenosRecomp) as diffs against upstream |
| `pgo/` | The PGO profile the released NRO was built with |
| `mesa/` | Which NVK driver build 1.0.0's renderer links (`-DFUTARI_SWITCH_NVK=ON`) |
| `docs/` | Documentation |

</div>

## Credits

- **CAVE** — creators of Mushihimesama Futari. This is an unofficial, fan-made port with no affiliation.
- **ChanseyIsTheBest** — this port.
- **[Tom Clay](https://github.com/rexglue/rexglue-sdk)** — the ReXGlue SDK, built on the work of the
  **[Xenia](https://xenia.jp)** team.
- **[StevensND](https://github.com/StevensND/nfsmw-nx)** — nfsmw-nx: the Switch layer of the SDK, the NVK build and
  much of the method; and [nfsmw-nx-installer](https://github.com/StevensND/nfsmw-nx-installer), the installer page
  this port's is built on.
- **[nebadasSwifty](https://github.com/nebadasSwifty/masseffect-nx)** — masseffect-nx: the native renderer this port's
  renderer grew from.
- **[hedge-dev](https://github.com/hedge-dev/XenosRecomp)** — XenosRecomp, the Xenos shader translator.
- **reNut and [masterspike52](https://github.com/masterspike52/reNut)** — reNut's Direct3D-level renderer, which the
  deko3d renderer is (reused with its author's permission); and Banjo-Kazooie: Nuts & Bolts' port (banjo-nx), whose
  adaptation of it, plume deko3d back end, shader passes and tools this port takes.
- **[plume](https://github.com/renderbag/plume)** (from UnleashedRecomp), **[deko3d](https://github.com/devkitPro/deko3d)**
  and **[uam](https://github.com/devkitPro/uam)** by fincs, with SwitchWakerHD's uam patches.
- **[danfromtico](https://github.com/danfromtico/mesa-switch)** and **[NaGaa95](https://github.com/NaGaa95/mesa-switch)**
  — mesa-switch: Mesa, NVK and NAK on Horizon.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)** — devkitA64 and libnx.
- The sibling ports ([Sonic Adventure 2](https://github.com/ChanseyIsTheBest/sa2-nx), Banjo-Kazooie: Nuts & Bolts,
  Sonic & All-Stars Racing Transformed, Peggle 2), which share their fixes with this one; Peggle 2's port (peggle2-nx)
  for the installer's deko3d shader chain and Renut's translator front end without DXC.
- **[KhronosGroup](https://github.com/KhronosGroup/SPIRV-Cross)** — SPIRV-Cross.
- The other libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Legal

No affiliation with CAVE, Microsoft or Nintendo. "Mushihimesama Futari" is a trademark of CAVE Interactive. This
repository contains no assets or program code from the original game. Users must provide their own legally obtained
copy, and everything made from it (`assets/`, `app/generated/`, the shaders) is made on their own computer.
Running homebrew requires custom firmware, which violates Nintendo's ToS and can get a console banned — your call.

Source code is provided under the GPL-3.0 License (see [LICENSE](LICENSE)), like the ports it builds on. The SDK keeps
its BSD-3-Clause license (see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).
