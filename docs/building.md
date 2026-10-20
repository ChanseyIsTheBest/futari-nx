# Building futari-nx

How to build the Windows PC build (used for development and testing), the Nintendo Switch NRO, the shader package and
the SD card folder, from your own copy of the game. To just play it, use the installer page instead (README).

## What you need

- **Your own Xbox 360 copy of Mushihimesama Futari Ver 1.5:** the disc image (`.iso`). The Black Label and Ver 1.01
  DLC (marketplace packages of title `435A07D7`, content type `00000002`) are optional.
- **Windows 10/11** with:
  - VS 2022 Build Tools (the MSVC libraries and the Windows SDK; CMake and Ninja come with it);
  - LLVM clang 20 (a portable copy is fine; by default `..\_tools\llvm` or the sa2-nx tree's);
  - Python 3 (with numpy for the comparison tools) and Git.

  `tools/win/env.ps1` finds them; override the paths with `FUTARI_LLVM`, `FUTARI_VSDIR` and `FUTARI_CMAKE_DIR`.
- **For the Switch:**
  - [devkitPro](https://devkitpro.org) with devkitA64 and libnx;
  - the NVK Vulkan driver built from [mesa-switch](https://github.com/danfromtico/mesa-switch), as a static
    `libvulkan.a` (nfsmw-nx's build; see `mesa/README.md`);
  - glslangValidator (`tools/win/build_glslang.ps1` builds it into `out/glslang`).

Run every build command from PowerShell, through `tools\win\run.ps1` or the scripts below (they load
`tools\win\env.ps1`). Running `cmake` from a Bash shell picks up MSYS2's tools: a configure started there fails and
wipes the build folder's cache.

## 1. Extract the game

```powershell
python tools\fetch_thirdparty.py                         # third-party sources of the SDK (ReXGlue SDK v0.10.0)
python tools\extract_iso.py "<your disc>.iso" -o assets\game_root
# Optional DLC, installed by the game at its first start: the package files in out\pc\dlc\, or unpacked folders in
# out\pc\dlc\00000002\<package name>\
```

## 2. Translate the game's code to C++

```powershell
powershell -File tools\win\run.ps1 out\host_configure.log "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
powershell -File tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue"
powershell -File tools\codegen.ps1 -Perf                 # default.xex and the 8 game cores -> app\generated (git-ignored)
```

`tools\codegen.ps1` also writes the decompressed images of the nine executables (`out\*_image.bin`), which the shader
tools and several post-codegen steps read. `-Perf` runs the post-codegen steps (`tools\post_codegen.py`: guest
memory without volatile, loop barriers, direct calls, the steps of the CPU rounds of 1.0.1 and of banjo-nx's codegen
round; docs/futari-notes.md); the build runs them again by itself after every code generation, and a second run
changes nothing. They take about an hour for the nine modules (three at a time, `FUTARI_POST_CODEGEN_JOBS`). The
generator's options are in `app/perf_overrides.toml`; every post-codegen step has a CMake switch back
(`FUTARI_GEN_*`, `FUTARI_SWITCH_*` in `app/CMakeLists.txt`). `-Force` regenerates after a change of the generator
itself. Three steps apply only to the functions measured to compile cheaper with them (`app/locals_optout`,
`app/codegen_choice`); `tools\codegen_choice.py` measures them with the Switch build's compiler (a new list needs a
fresh `-Force` codegen).

## 3. The PC build

```powershell
powershell -File tools\win\run.ps1 out\pc_configure.log "cmake --preset win-pc -S app"
powershell -File tools\win\run.ps1 out\pc_build.log "cmake --build out/pc -j 4"
powershell -File tools\win\boot_test.ps1                 # muted and minimized, then the end of the log
```

`out\pc\futari.toml` holds the PC's settings; test runs are muted and minimized. `-VideoAt <seconds>` and
`-VideoFrames` record the game image to `out\pc\captures\` (ffmpeg at `C:\ffmpeg\ffmpeg.exe`). With
`REX_VBLANK_PER_SWAP=true` the game advances one frame per presented swap (lockstep), and the frame hashes of two runs
can be compared with `tools\compare_frames.py`.

## 4. The shader package

The native renderer draws with the game's shaders translated ahead of time (XenosRecomp to HLSL, DXC to SPIR-V) into
`futari_shaders.mesp` + `futari_shaders.mesp.idx`. Futari keeps every shader inside its executables, so no play
session is needed:

```bash
python tools/extract_shaders.py --out out/shader_dump                      # the containers in the images (42)
python tools/sweep_raw_shaders.py out/*_image.bin --out out/shader_sweep  # the raw microcode block (17)
python tools/wrap_discovered.py --raw out/shader_sweep --containers out/shader_dump --out out/shader_wrapped
tools/build_shader_library.sh out/shader_dump out/shader_wrapped          # 55 shaders -> out/shaders/
```

**The released library is the one the installer page makes** (the same steps in JavaScript, with its WebAssembly
translator, DXC and packer: `node test/library_node.mjs <game> --out <folder>` in futari-nx-installer). The native
DXC can produce different SPIR-V for the same HLSL, so test an NRO for release with the installer's library. The
installer checks its library against the SHA-256 in its `release/manifest.json`.

`shaders/wasm/build_wasm_tools.sh` builds the installer's translator, packer and LZX decoder with Emscripten.

## 5. The Switch NRO

```powershell
powershell -File tools\win\build_switch.ps1 -Jobs 3      # out\switch\futari.nro (LTO: allow 20-30 minutes)
python tools\check_switch_build.py                       # start-up code at 0, the SDK's Mesa, no vector FMA, no
                                                         # self-indexed table lookup, no fcvtas in game code
python tools\find_self_loops.py out\switch\futari        # no branch-to-self loops in game code
powershell -File tools\win\package_switch.ps1            # the SD card folder: out\switch_sd\switch\futari-nx
```

`build_switch.ps1` configures `out\switch` the first time (`-BuildDir` builds elsewhere). The NRO's name, author,
version and icon (`app/futari_icon.jpg`) are in `app/CMakeLists.txt`. The settings compiled into the NRO are
`app/futari_switch_defaults.toml` (written by `tools/make_enhancement_configs.py` from the enhancement table of
`app/src/futari_enhancements.cpp`), and the player's `futari.toml` is `app/futari_switch.toml`. Hot functions are
placed first (`app/hot_functions.ld`, from console profiles with `tools/hot_order.py`).

### Profile-guided optimisation

`pgo/` holds the profile the released NRO was built with (one console session of version 1.0.0). To build with it:

```powershell
powershell -File tools\win\build_switch.ps1 -Pgo use
```

`-Pgo none` goes back to a build without PGO. The profile matches the code of this repository: a function you change
loses its part of it, and the rest still applies. The files are named by the object's path in the build folder (with
`#` for each separator), so build in `out\switch` for them to apply.

To record a new profile:

1. `build_switch.ps1 -Pgo generate -BuildDir out\switch_pgo` builds an instrumented NRO. Play it on the console: it
   writes `pgo/futari_pgo_<N>.stream` next to the NRO (one file per session, saved every 2 minutes, on the HOME button
   and at exit).
2. Move the `.gcda` files of `pgo\` aside, copy the console's `pgo` folder to the PC, and run
   `powershell -File tools\win\pgo_merge.ps1 <folder>`. It merges every session into `pgo\` and fills in each file's
   summary. Merge on the PC that built the instrumented NRO: the streams name its profile folder.
3. `build_switch.ps1 -Pgo use` builds the optimised NRO.

Do not change the game or app code between steps 1 and 3: a changed function loses its profile.

## 6. The deko3d renderer (Renut's, the Switch default)

Renut's Direct3D-level renderer (`app/src/renut_engine/gpu`; `futari_renderer = "renut"`, `renut_gpu_backend =
"deko3d"` on the Switch, `"vulkan"` or `"d3d12"` on the PC) is built in by default (`-DFUTARI_RENUT_RENDERER=OFF`
leaves it out). Its hooks name Futari's Direct3D functions; after every codegen:

```bash
python tools/renut_futari_hooks.py      # the hook sites of default.xex and the eight cores (futari_renut_hooks.h,
                                        # futari_renut_core_hooks.cpp); then tools/post_codegen.py if it rewrote them
python tools/perf_codegen_post.py --check   # must report nothing to rewrite (every hooked call goes through its hook)
```

Shaders, once and after a translator change (game data: `out/` only):

```bash
python tools/renut_dump_shaders.py      # the 42 shader containers of the nine images -> out/renut_shader_dump
# the PC build (section 3) translates them with Renut's XenosRecomp and banjo-nx's early alpha copies
bash tools/renut_deko3d_shaders.sh      # deko3d: SPIR-V -> GLSL -> banjo-nx's passes -> the NRO's package and cache
python tools/deko3d_variant_cache.py --list out/pc/pipeline_list.bin --glsl out/deko3d_shaders/glsl_opt     --out out/renut_switch/deko3d_shaders.bin        # the DKSH cache, next to the NRO on the SD card
python tools/renut_release_shaders.py   # the release build's files: the shader table without SPIR-V, and
                                        # out/renut_switch_release/sd/deko3d_glsl.bin (the Python tools' copy, for tests)
```

The NRO carries no game shader (`FUTARI_RENUT_RELEASE_SHADERS`, on by default since 2026-10-20, peggle2-nx's design):
it reads `deko3d_glsl.bin` beside itself, which the web installer makes from the player's executables in the browser
(futari-nx-installer's `lib/deko3d.js`: the same chain in WebAssembly and JavaScript; 40 of the 42 shaders' GLSL
identical to the PC tools', the other two differ only where the page's newer DXC numbers its values,
`node test/deko3d_node.mjs <game> out/deko3d_shaders/glsl_opt` there). For tests put the installer's file or
`out/renut_switch_release/sd/deko3d_glsl.bin` next to the NRO; without one the log says how many of the game's
shaders are missing and nothing of the game is drawn. `-DFUTARI_RENUT_RELEASE_SHADERS=OFF` builds every translated
shader in (development: `out/renut_switch`).

The deko3d host tools (`out/deko3d_tools`: SPIRV-Cross and the PC uam) come from `tools/deko3d_build_tools.sh`.
For the Switch build, set `TMP` and `TEMP` to a folder on D: when C: is short of space (the LTO link writes several
GB of temporaries). Ryujinx runs the NRO (ChanseyIsTheBest's build): put it, `deko3d_glsl.bin`, `deko3d_shaders.bin`,
`futari.toml` and `game_root` in `sdmc:/switch/futari-nx` (the SDK falls back to that folder when the emulator gives no argv). Its
Ryujinx.HLE.dll of 2026-10-20 11:23 (banjo-nx's) signals the display's vsync on a 60 Hz grid as the console does;
with an older one set `futari_vblank_display_sync = false` (it signalled after each composition).

The Switch NRO links no Vulkan driver (`FUTARI_SWITCH_NVK`, default OFF: Mesa's NVK out of the link,
`sdk/src/ui/switch_vulkan_stub.c` answering the entry points the code references), so only Renut on deko3d draws
there. `-DFUTARI_SWITCH_NVK=ON` (in the build folder: `cmake out\switch -DFUTARI_SWITCH_NVK=ON`) links the driver
again for the native renderer (`futari_renderer = "default"`) or Renut on NVK (`renut_gpu_backend = "vulkan"`).

Renut's optimisations are rows of `app/src/futari_enhancements.cpp`'s table, so the two masters switch them: a key
taken from banjo-nx later needs its row (and `python tools/make_enhancement_configs.py`), except an experimental one,
which honours the master in code instead (a row puts it on in the built-in settings). `futari_cpu_verify = true`
also checks Renut's delta captures draw by draw: the `[renut] delta captures` line must say 0 mismatches.

## Tests

| Script | What it does |
|---|---|
| `tools\win\boot_test.ps1` | One muted, minimized run; `-Pad` scripts the controller, `-VideoAt` / `-VideoFrames` record it |
| `tools\make_enhancement_configs.py` | The test configurations (`app\configs`) and the built-in settings |
| `tools\compare_frames.py` | Lockstep frame-hash comparison (`--any` against several baselines) |
| `tools\declick_test.cpp` | The Switch audio declick run on a PC audio dump (`audio_dump_s`) |
| `tools\check_switch_build.py`, `tools\find_self_loops.py` | The checks every Switch build must pass |
| `tools\win\run_sdk_tests.ps1` | The SDK's unit tests of the generated code's helpers (conversions, float copies, vector helpers) |
| `tools\switch_compile_check.py`, `tools\arm_forms_check.py` | Chosen sources compiled with the Switch compiler and its flags, without building the NRO; the AArch64 forms the notes claim |
| `tools\read_before_write.py`, `tools\non_argument_audit.py` | The audits behind the generator's register-local options (run them after a change of the overrides) |

The notes of every step, measurement and finding are in [futari-notes.md](futari-notes.md).
