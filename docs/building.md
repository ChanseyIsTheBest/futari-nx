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
powershell -File tools\codegen.ps1                       # default.xex and the 8 game cores -> app\generated (git-ignored)
```

`tools\codegen.ps1` also writes the decompressed images of the nine executables (`out\*_image.bin`) used by the shader
tools, and the build runs the post-codegen passes (`tools\perf_codegen_post.py`: direct calls, guest memory without
volatile) after every code generation.

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
python tools\check_switch_build.py                       # start-up code at 0, the SDK's Mesa, no vector FMA
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

## Tests

| Script | What it does |
|---|---|
| `tools\win\boot_test.ps1` | One muted, minimized run; `-Pad` scripts the controller, `-VideoAt` / `-VideoFrames` record it |
| `tools\make_enhancement_configs.py` | The test configurations (`app\configs`) and the built-in settings |
| `tools\compare_frames.py` | Lockstep frame-hash comparison (`--any` against several baselines) |
| `tools\declick_test.cpp` | The Switch audio declick run on a PC audio dump (`audio_dump_s`) |
| `tools\check_switch_build.py`, `tools\find_self_loops.py` | The checks every Switch build must pass |

The notes of every step, measurement and finding are in [futari-notes.md](futari-notes.md).
