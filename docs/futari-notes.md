# Mushihimesama Futari Ver 1.5: notes for the port

## The disc

| | |
|---|---|
| Title | `435A07D7` (CAVE), media `60A28AE5`, XGD2, game partition at `0x0FD90000` |
| Files | 54 files, 849 MB (`tools/extract_iso.py`) |
| `default.xex` | `cave360.exe`, 2,760,704 bytes, XEX2 AES + basic compression, version 0.0.0.1, built 2009-10-11 |
| Image | base `0x82000000`, size `0x002B0000`, entry `0x8208A2B8`, `.text` `0x82060000`+`0x175BF4` |
| XDK | 2.0.9328 (D3D9, XACT3, XAudio2, XMP, XOnline) |
| DLC | marketplace packages (content type `0x00000002`): Black Label (`Rom/cg_black.bin`, `cg_blackhi.bin`, `dropprm.bl`, `spa.bin`) and the Ver 1.01 key (`ver101_key.bin`) |

Data: `media/Rom/cg_v102{,hi}.bin` and `cg_v154{,hi}.bin` (graphics of the 1.01 and 1.5 arcade boards, normal and high
resolution), `media/wall/wp01-13.bin` (wallpapers), `media/Sound/*.xwb/xsb/xgs` (XACT), `media/motion/*.lzx` and
`media/font/*.lzx` (CAVE's own psb/m2 formats: "motion file '%s' ... update MotionEditor").

## Modules: one front end, eight game cores

`default.xex` is only the front end: menus, saves, DLC (`DLCControl`), achievements, storage. The game itself runs
in a **game-core DLL** from `media/Module/`, loaded with `XexLoadImage` when a game starts (`game:\media\Module\` +
name + `.xex`, logged as `DLL %s`):

| Module | Image | `.text` |
|---|---|---|
| `palm_normal.xex` | `0x88000000`+`0x910000` | `0x88070000`+`0x1E2560` |
| `palm_abnormal.xex` | `0x88000000`+`0x7D0000` | `0x88080000` |
| `reco_normal.xex`, `reco_abnormal.xex` | `0x88000000`+`0x7B0000` | `0x88070000` |
| `hirow_normal.xex`, `hirow_abnormal.xex` | `0x88000000`+`0x7B0000` | `0x88070000`+`0x1DBE30` |
| `kiniro_normal.xex`, `kiniro_abnormal.xex` | `0x88000000`+`0x7B0000` | `0x88070000` |

Module flags `0x9` (title module + DLL), imports only `xboxkrnl.exe`, export table `.edata` (`0x5F` bytes). Each links
its own copy of LIBCMT, **D3D9**, XGRAPHC, XAPILIB (XDK 2.0.9328). Reco and Palm are the two characters, Normal and
Abnormal their styles ("Reco/Normal", "PANEL_PALM_A" in `default.xex`). The modes the front end boots are logged as
`Bootup MODULE_154_3`, `MODULE_154_3_HI`, `MODULE_ARRANGE`, `MODULE_EASY`, `MODULE_BLACK`, `MODULE_BLACK_HI`,
`MODULE_102`, `MODULE_102_HI` (Ver 1.5, Arrange/Xbox 360 mode, Novice, Black Label, Ver 1.01; `_HI` = high-resolution
graphics); which core each one loads is still to be traced (hirow and kiniro are not character names).

All eight are built for the **same base address**, so the console only ever holds one. Consequences:

- Code generation: ReXGlue loaded every DLL of a manifest at once and refused overlapping ones. `sdk/src/codegen/
  project_recompiler.cpp` now copies each DLL into a `BinaryView` and unloads it before loading the next, and only
  the entrypoint must stay clear of the DLLs. All nine binaries translate (17 s, ~275 MB of C++).
- Run time: `KernelState::LoadUserModule` / `UnloadUserModule` already register and tear down a module's function
  table, so cores can follow one another at the same address.
- PC: each core is a host DLL (`futari_<core>.dll`, ReXGlue's default for guest DLLs).
- Switch: no shared libraries. Each core is an OBJECT library (template `dll_targets_cmake.inja`, Switch branch);
  `rexglue_switch_static_module` (`sdk/cmake/rexglue_switch.cmake`) partially links its objects (`g++ -r
  -Wl,--force-group-allocation`, so inline/template copies are resolved inside the core), makes every hidden symbol
  local (`objcopy --localize-hidden`; the generated code is built with hidden visibility, as for a DLL) and renames
  `ReXModule_Register` / `ReXModule_GetImageInfo` to `..._<core>`. A generated source registers them with
  `KernelState::RegisterStaticModuleLibrary("futari_<core>", ...)`; `LoadUserModule` uses a registered library instead of
  `DynamicLibrary::Load`. The same eight `sub_88XXXXXX` symbols per core therefore never meet at link time (the
  toolchain links with `--allow-multiple-definition`, which would otherwise silently keep one core's code for all).
- Switch FFmpeg: `sdk/thirdparty/FFmpeg/config.h` lost nfsmw-nx's `__SWITCH__` branch in the sa2-nx copy
  ("#error no config"); restored.
- First NRO (2026-10-05): `out/switch/futari.nro`, 93.6 MB, `tools/win/build_switch.ps1 -Jobs 3` (≈2.5 h from
  scratch at 3 jobs; the GCC build tree takes ≈4 GB). Checked in the ELF: 8 `ReXModule_Register_futari_<core>`, every
  `sub_88xxxxxx` local, none global. `$<TARGET_OBJECTS>` lists GCC's `cmake_pch.hxx.gch`, which `ld -r` cannot read:
  filtered out. `tools/win/package_switch.ps1 -WithGame -Dlc <folder>` assembles `out/switch_sd/`. Switch defaults
  (`app/futari_switch.toml`): native renderer, and the three ring-thread switches `futari_native_fast_pair`,
  `_raw_microcode`, `_pm4_fast` (exact by construction, self-checked; no DIFFERENCE on Futari's PC runs).

## Static sweep before booting (2026-10-05)

| Class | Tool | Result |
|---|---|---|
| Functions reached only by pointer | `tools/pointer_fixpoint.py --module <m>` (now per binary: image base, generated tree and `overrides_<core>.toml` come from `--module`) | `default.xex`: 48 entries (16 virtual-call thunks `lwz r12,0(r3); lwz r11,N(r12); mtctr; bctr`, 22 this-adjustor thunks `addi r3,r3,-N; b`, vtable methods, 1 callback). Cores: 8-11 each, all callbacks built with `lis`/`addi`. Fixpoint after one pass everywhere. |
| Jump tables | `tools/switch_check.py` | `default.xex`: 35 switches, none truncated or missed; 3 odd shapes (a 22-case switch at `0x820EB278` the tool cannot size, two struct-held indirect calls). Each core: one 2-case table with no bound in the code (`sub_880EBB80` in palm_normal, the same function in all cores). |
| Other code pointers | `tools/pointer_audit.py --module <m>` | the vtable-like and built-in-code hits are exactly the fixpoint's entries |
| `setjmp`/`longjmp` | by shape | one pair, in `default.xex`: `setjmp` `0x8215B3E0`, `longjmp` `0x8215AFA0` (`overrides.toml`); none in the cores |

The generator fixes from sa2-nx (`straightLineSwitchBound` for jump-table bounds, the `[[switch_tables]]` key warning,
per-DLL image dumps with `REXGLUE_DUMP_IMAGE`) are in `sdk/src/codegen` too.

## What the first boots needed

| Boot | Stopped at | Cause | Fix |
|---|---|---|---|
| 1 | 7.5 s, "Call to invalid or unregistered function at guest address `0x821CB190`" | a callback built with `lis`/`addi` at `0x821CB1F0` | the pointer fixpoint (above) |
| Switch 1 | 3 s, "Unhandled guest access violation: read of guest `0x4C`" on thread `F8000048` | thread start race: `sub_82191640` starts `sub_821911D0` at once, which reads the manager at `0x821F8588` and its member `+76` before the main thread has filled it in (the host load of `[0x4000A44C]` returned 0). The 360 and the PC never lose this race; the Switch's cores did | `app/src/futari_fixes.cpp`: the thread routine waits (≤ 10 s) for both, then runs unchanged; no wait on PC |
| Switch 1 | DLC not installed: "InstallUnpackedContent: copying ... failed" | `std::filesystem::copy(recursive)` fails on Horizon's newlib | file-by-file stdio copy in `ContentManager::InstallUnpackedContent` |

## Shaders

The shader containers are in the images (`tools/extract_shaders.py`): 22 vertex + 22 pixel in `default.xex`, 2 + 2
in each core (the same 4 in all eight), 42 distinct. `tools/build_shader_library.sh out/shader_dump` translates all
42 (XenosRecomp, with sa2-nx's fix: interpolators always linked by hardware register) and compiles them with DXC:
`out/shaders/futari_shaders.mesp` (+ `.idx`), 42 shaders, no run-time dump needed.

## Native renderer (stage 4, PC)

`futari_renderer_native = true`: the PM4-ring Vulkan renderer of masseffect-nx/banjo-nx/sa2-nx (`app/src/me`), no
game-specific hooks. Results on 2026-10-05 (scripted run title → menus → Ver 1.5 Arcade → stage 1):

- **Shaders.** Futari draws mostly with shaders that have no container: raw microcode in `.rdata` (`0x820032C8`
  and on in `default.xex`, at the same place in every core), loaded with `IM_LOAD_IMMEDIATE`. A discovery run
  (`FUTARI_SHADER_DISCOVERY`, `FUTARI_SHADER_MISSING`, `FUTARI_VERTEX_VARIANTS` = folders) dumped 9 VS + 1
  PS; `tools/wrap_discovered.py` wrapped 8 (2 VS are covered by containers the renderer materialized), and
  `tools/build_shader_library.sh out/shader_dump out/pc/shaders_missing out/pc/shaders_wrapped` packs 52 shaders. The
  9-word PS (`oC0 = r0`) also sits in two real containers with different metadata, so its microcode hash was ambiguous
  and dropped; the wrapper (register-linked) is what the code index now picks. Log: 0 unidentified loads.
- **Sprites** are rect lists drawn with integer pixel coordinates: `k_32_32` signed integer fetches scaled by VS
  `c255 = (2, 1/256, -0.5, 0)` into a 640x512 target. Renderer changes (`futari_native_draws.cpp`): `k_32*` read as
  floats are converted on upload (`RepackWords32`, was rejected with cause 334), and the conversion is applied to all
  three corners of an expanded rectangle (was only the first: every sprite collapsed to a point).
- **Arcade screen chain**: 640x512 k_8_8_8_8 (sprites) → resolve → 320x240 k_2_10_10_10_FLOAT target → resolve to a
  k_2_10_10_10 texture → drawn into the 1280x720 frame (the centre panel); sprite sheets and caches are resolved into
  `k_1_5_5_5` textures (CAVE's 15-bit colour). Renderer changes (`me_formats_color.h`, `futari_native_targets.cpp`):
  `k_1_5_5_5` / `k_5_6_5` resolve formats (were rejected with cause 303) and the resolve's bytes per texel from the host
  format (`Log2TexelResolved`; was 4 for every non-64-bit format).
- **Comparison with the emulated GPU** (`tools/compare_runs.py`, two raw recordings of the same scripted run): title,
  save dialog, menus and the gameplay frames before the two runs' game states drift apart are **pixel-identical**
  (median difference 0.00 over 900 gameplay pairs). Remaining differences seen are game-state (animation phase,
  score) because input timing is wall-clock based.
- Diagnostics added: `futari_dump_resolves_s` (every resolved image of one frame to `captures/resolve_*.raw`,
  `tools/resolve_png.py`), `c255`/viewport/clip in the frame trace (`banjo_trace_frame_s`,
  `FUTARI_TRACE_ALL_DRAWS=1`). RenderDoc needs `VK_KHR_portability_enumeration` off (Mac-only now, banjo-nx's fix).
- GPU choice: D3D12 and Vulkan auto-selection now prefer the discrete GPU (RTX 3060 Laptop over the AMD iGPU).
- Blend operation 4 (Xenos reverse subtract) is now `VK_BLEND_OP_REVERSE_SUBTRACT` (futari mapped it to SUBTRACT;
  found by banjo-nx). Not seen in Futari's frames so far, but any effect using it would have been wrong.

### Shader coverage without playing every mode (2026-10-06)

- **D3D-patched vertex shaders.** D3D rewrites every vertex fetch for the bound declaration, destination swizzle
  included (float3 `0x688`→`0xA88` = xyz1, D3DCOLOR →`0x60A` = zyxw, float2 →`0xB08` = xy01). `ShadersNative::Identify`
  fingerprinted with the swizzle kept, so each declaration needed its own hand-wrapped variant (6 of the 52 shaders).
  Identify now has a second pass with the draw-time predicate (`me_shader_identity.h` `VertexShaderIdentityMatches`:
  only fetch fields differ, swizzle representable by `g_InputRemap`), closest candidate first. Without the 6 variants:
  0 unidentified loads, title/menus pixel-identical to the old package.
- **Raw shaders** (`IM_LOAD_IMMEDIATE`, no container): `tools/sweep_raw_shaders.py` finds them statically in the
  module images by control flow. Size rule: (instructions + 1) × 3 words, confirmed on all 4 seen at run time. 19
  programs; 8 sit inside real containers, 9 had never been loaded by a run → the package now has **61** shaders
  (`out/shaders_61`; previous 52 in `out/shaders_good`). Data files (game + DLC) hold no shader containers at all.
- **Console safety net:** cvar `futari_shader_misses` (on in `futari_switch.toml`: `shader_misses`) writes each
  shader the package lacks to the SD once; wrap it with `tools/wrap_discovered.py`, rebuild the package, no new NRO.
- Compared with UnleashedRecomp: XenosRecomp scans every game file for `0x102A11xx` containers offline and the game's
  CreateVertexShader/CreatePixelShader are hooked at API level (never patched microcode); a miss is an empty shader.
- Peers (2026-10-06): Banjo, SA2, Peggle 2 and Racing Transformed all hit the patched-VS issue; they solved it with
  per-declaration variants (`make_vertex_variants.py` + `repair_vertex_variant_declarations`). The PS export-mask walk
  in `wrap_discovered.py` comes from banjo-nx.

### Module swaps (Xbox 360 mode crash on the Switch, 2026-10-06)

`UnloadUserModule` looked the recompiled code up by `module->path()` (the resolved VFS path), which never matched the
manifest's guest path, so `XexUnloadImage` left the old core registered and the next core at 0x88000000 failed with
"overlaps existing" (Arcade = `reco_normal` worked; Xbox 360 mode = `reco_abnormal` crashed). Fix (found by the crash
agent): the key is recorded at load (`UserModule::recompiled_key`, also on the Switch's statically linked path) and the
DLL load no longer resets the image heap. Also merged: overlapped `XEnumerate` completes with `ERROR_FUNCTION_FAILED`
(saving never worked), and unpacked DLC gets the display name `DLCControl` compares (Black Label greyed out).

## Direct3D (XDK 2.0.9328), for the native renderer

`tools/d3d_match.py` against Banjo-Kazooie: Nuts & Bolts' named functions (XDK 2.0.7645): same instruction/register
shape, immediates ignored (1.00 = identical shape). In `default.xex` (first pass, 2026-10-05; "check" = not yet
confirmed):

| Function | Futari `default.xex` | Score |
|---|---|---|
| `D3DDevice_CreateVertexShader` | `sub_8209B498` | 1.00 |
| `D3DDevice_CreatePixelShader` | `sub_8209B2B0` | 0.59 (check) |
| `D3DDevice_CreateTexture` | `sub_8209D2E0` | 1.00 |
| `D3DDevice_DrawVertices` / `DrawIndexedVertices` | `sub_8209D800` / `sub_8209DCE0` | 0.76 / 0.65 (check: BeginVertices also matches `sub_8209D800`) |
| `D3DDevice_DrawVerticesUP` | `sub_8209DC98` | 0.89 |
| `D3DDevice_Clear` | `sub_820A19F0` | 0.96 |
| `D3DDevice_Resolve` | `sub_820A2280` | 0.67 (check) |
| `D3DDevice_Swap` | `sub_820A5408` | 0.66 (check) |
| `D3DDevice_SetGammaRamp` | `sub_82099520` or `sub_82099450` | 1.00 (identical pair) |
| `D3DDevice_SetShaderGPRAllocation` | `sub_8209B098` | 0.97 |
| `D3DDevice_SetRingBufferParameters` | `sub_820A92E0` | 0.83 |

Every module has its own D3D9 too (see `out/d3d_match_<core>.txt`).

## Crash audit (2026-10-05)

Done in a copy of the tree (futari-nx-crash), Xenos GPU, runs minimized and muted, own user data folder.

**Board -> core.** The front end's board boot state `sub_821C3B90` news the loader for the index at `this+104`
(`sub_821C2F58`... one per core, each `XexLoadImage("game:\media\Module\<core>.xex")` + `XexGetProcedureAddress(1)`);
confirmed by the "Registering module" lines:

| TOP MENU entry | panel (left/right) | index | core |
|---|---|---|---|
| Ver1.5 | Arcade / Xbox360 | 0 / 1 | `reco_normal` / `reco_abnormal` |
| Arrange | Arrange / Novice | 2 / 3 | `palm_normal` / `palm_abnormal` |
| Black Label | Arcade / Xbox360 | 4 / 5 | `kiniro_normal` / `kiniro_abnormal` |
| Ver1.01 | Arcade / Xbox360 | 6 / 7 | `hirow_normal` / `hirow_abnormal` |

Inside a board the cabinet runs its attract (warning screen, title, ranking); A opens the cabinet menu: SCORE ATTACK,
NORMAL PLAY, TRAINING, REPLAY DATA, OPTION, CLOSE, RETURN TO TOP MENU. Pause (start) in a game: RESUME, RESTART,
OPTION, RETURN TO TITLE. The character and shot type are chosen inside the core; they do not change the core.

**Found and fixed**

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Second board after RETURN TO TOP MENU: "Module image range [88000000, ...) overlaps existing", `XexLoadImage` fails, then "[FATAL] Call to invalid or unregistered function" (0x1F / garbage) | The game does unload the old core first (loader dtor `sub_821C8A18` -> `XexUnloadImage`, on RETURN TO TOP MENU), but `KernelState::UnloadUserModule` looked the recompiled code up by `module->path()`, the resolved VFS path (`\Device\Harddisk0\Partition1\...`), which never matches the manifest key: function table, `module_tables_` entry and host library stayed | `UserModule::recompiled_key()` set at load, used by the unload (`user_module.h`, `kernel_state.cpp`) |
| 2 | (same scenario, hidden by 1) the new core's image was decompressed over the still-registered old one | `XexModule::ReadImage` reset the page table of the whole 0x80000000 heap for every image, DLLs too, so `AllocFixed` over a loaded module "succeeded" | Reset only for executables (`xex_module.cpp`) |
| 3 | Saving never worked: "save data not found, create?" -> yes -> "creation cancelled"; replays not saved either | An overlapped `XEnumerate` that reaches the end completed with ERROR_NO_MORE_FILES (0x12); the game (`sub_82194438`, `sub_82192B08`) takes only 1627 as end of list, anything else as a storage error (result 9) | Overlapped failures complete with ERROR_FUNCTION_FAILED, real error in the extended error, as on the console / xenia (`xam_enum.cpp`) |
| 4 | Black Label greyed out, Ver1.01 entry missing | DLCControl identifies a package by `XCONTENT_DATA.szDisplayName` (wcscmp in `sub_82192B08` against the table at `0x821EC988`: `0x82046958` "虫姫さまふたり　ブラックレーベル", `0x8204697C` "虫姫さまふたり　Ver1.01"); unpacked packages were installed with their folder name as display name | `futari_dlc.cpp` names unpacked packages from their files (`Rom/cg_black.bin`, `ver101_key.bin`) and rewrites old headers at start. Black Label mounts as `bldata:` and runs |
| 5 | Leaderboard (risk, offline it is greyed out): `XamUserCreateStatsEnumerator` was a stub, the game ignores its result and does `malloc(*size)` + `XEnumerate(*handle)` on uninitialised stack words (`sub_8218FFC8`), then reads `pViews` (`sub_8218FDB0`) | stub | Real enumerator returning empty views (`xam_user.cpp`) |
| 6 | Stubs the game imports: `XNetLogonGetTitleID` (returned its argument, 7), `XamBackgroundDownloadSetMode`, `XamShowMarketplaceDownloadItemsUI` (left the XOVERLAPPED/HRESULT untouched; called from `sub_821B79A0` when a DLC is offered), `XamShowGamerCardUIForXUID` | stubs | implemented (`xam_misc.cpp`, `xam_ui.cpp`) |

**Static classes.** C++ exceptions: `default.xex` throws only from the STL string checks (`_CxxThrowException` =
`sub_8207CA00`, called by `sub_8207A250` "string too long" and `sub_8207A2D0` "invalid string position"); the other
`RtlRaiseException` callers are the heap (`HEAP_GENERATE_EXCEPTIONS` only), the FP-exception path of the math
library (`sub_82083FB0`, unmasked FP exceptions only) and SetThreadName. The cores never throw (their only raises are
SetThreadName `sub_8808F678` and the same FP path). Jump tables: the "2-case table with no bound" of every core
(`sub_880EBB80`, bctr `0x880EBCAC`) is a 10-case switch bounded by `cmplwi r10,9; bgt` and copied with
`rotlwi r11,r10,0`; `tools/switch_check.py` now treats that as a copy and all nine binaries check clean (no truncated,
missed or unbounded table). Kernel imports still stubbed or partial and reached at boot: `XamShowMessageBoxUIEx`
(returns 0 without UI, only used by the XDK's video-mode check `sub_8208A0D0`); `XamSessionCreateHandle` /
`XamSessionRefObjByHandle` return fake objects (the ranking upload fails cleanly, see below).

**Runs** (logs clean of FATAL, unregistered calls, traps, exceptions, BaseHeap, STUB):
- all 8 boards loaded, unloaded and reloaded in one run (10 loads), and 10 alternating Ver1.5/Arrange swaps: working
  set levels off at ~630-900 MB, no growth per swap;
- gameplay (NORMAL PLAY, ~60 s, shot held, weaving) in each of the 8 cores, one run each; Ver1.01 Arcade reached the
  CONTINUE countdown;
- Black Label Arcade: `bldata` mounted, `cg_black.bin` / `dropprm.bl` read, game runs;
- save data created and reloaded; Score Attack to game over -> replay saved to a slot -> REPLAY DATA -> replay plays;
- pause menu, RETURN TO TITLE, RETURN TO TOP MENU; 15 min in one board with a scripted pad (no crash).

**Behaviour that looks like a hang but is not.** After a Score Attack the game asks to register the score; without
Xbox LIVE it then asks "access the ranking again?" for as long as the answer is A (yes); B ends it. Leaderboard and
the LIVE prompts are expected offline.

**Not tested.** TRAINING, OPTION changes and their saving, the continue itself (pressing start on CONTINUE?), 2P SIDE
START and a second pad (the scripted pad now has `p2:` steps for it), a 20-minute soak in gameplay, the HI cores'
long play. Codegen (`rexglue.exe`) was not rebuilt with the `xex_module.cpp` change; the generator unloads each DLL
before loading the next (`project_recompiler.cpp`), so it should not mind.

## CPU and GPU performance (2026-10-06)

Research in `docs/research/` (Unleashed/Marathon catalogues read in full, masseffect-nx's catalogue for this very
renderer and SDK, the lessons file, the verify/profiler note shared with the other ports).

**Enhancement toggles** (`app/src/futari_enhancements.cpp`): 63 enhancements (45 CPU, 18 non-CPU), each a cvar with
its ON and OFF value; `futari_cpu_enhancements` / `futari_noncpu_enhancements` masters force a group off; the startup
line `[enhancements]` lists what is active. The configs are generated from the table by
`tools/make_enhancement_configs.py`: default `futari.toml`, `configs/futari_cpu_on|cpu_off|cpu_verify|profile.toml`.

- Phase A (runtime): masseffect-nx's best-profile ring set (deferred recording, flat identity, fast pair/raw
  microcode/PM4, compare_fast, fast_copy, indices_cache, notify_no_lock, fingerprint mode 1, dedupe max 16384), the
  renderer's existing fast paths, SDK wait settings, GPU features, diagnostics off, handheld GPU 460.8 MHz.
  PC A/B (all CPU on vs CPU master off): title/menus pixel-identical, later frames only timing.
- **Verify mode** (`futari_cpu_verify`, `me_verify.h`): every fast path's self-check runs on every use (verify counts
  raised to 2^31-1); `[verify] checks done: ... | differences: N` every 10 s, N must be 0 (DIFFERENCE/MISMATCH sites
  counted, the prewarm timing guard excluded). PC: millions of checks per path, 0 differences.
- Profiler: `switch_perf.cpp` prints every sampled pc with its count; link map `out/switch/futari.map`;
  `tools/switch_hot_functions.py` ranks functions per thread (self time) with module labels.
- Phase B (build-time, A/B NRO pair in `out/switch_ab/`): `app/perf_overrides.toml` (cr/xer/ctr/reserved as locals,
  skip_msr, elide_lr_stores, dform_disp_split, fctiwz_inline), `perf_overrides_default.toml` (share_registers for the
  three setjmp callers), `tools/perf_codegen_post.py` (direct calls: ~178k; guest memory without volatile), run by
  `tools/codegen.ps1 -Perf`. One core: ctx.lr stores 23,843 -> 1,044, ctx.cr 73,377 -> 95, ctx.xer 49,287 -> 55.
  PC: runs to gameplay, verify 0 differences. Baseline generated code kept in `out/generated_baseline`.
- Phase B2 (not yet): `non_volatile_as_local` / `non_argument_as_local` need share_registers marks for the 35 + 13x8
  functions that read caller non-volatile registers (`tools/scan_caller_register_reads.py`, list in
  `docs/research/caller-register-reads.txt`).
- **Compile flags fixed** (2026-10-06, banjo-nx's finding): `-ffp-contract=off -fno-strict-aliasing` reached only
  `futari`/`futari_recomp`; the eight game-core targets (`futari_<core>`, OBJECT libraries on the Switch) had neither,
  so all gameplay code was built with GCC's FMA contraction (floating point drifting from the 360 and the PC) and
  strict aliasing (unsafe for recompiled code); the PC clang++ build lacked `-fno-strict-aliasing` too. Every NRO
  before 2026-10-06 03:40 has this. `app/CMakeLists.txt` now covers every core, for GCC and Clang.
- Fast vertex copy verify (banjo-nx's fix): a checked copy runs both paths on one snapshot of the guest source (a
  second read could see a dynamic vertex buffer the game rewrote in between: a false MISMATCH).
- **Post passes kept by every build** (2026-10-06): the build re-runs codegen whenever one of its inputs changes, and
  the first perf builds lost both post passes that way. `app/CMakeLists.txt` APPENDs `tools/perf_codegen_post.py` to
  the codegen command (option `FUTARI_PERF_CODEGEN_POST`, ON). The pass also turns direct calls back into hookable
  calls for an address hooked later (a new native replacement): run it after adding one.
- **PC profiler** (`app/src/futari_pc_profiler.cpp`, cvar `futari_pc_profile_s` = report every N s, `_hz`): a
  time-critical thread samples every busy thread (cycle counter moved) with Suspend/GetThreadContext/Resume and writes
  per-thread and overall self time per function (DbgHelp + the PDBs) to `logs/pc_profile.log`;
  `tools/pc_hot_functions.py` adds report windows up. For choosing native rewrites before a console profile.
- **Native replacements** (`app/src/native/`, UnleashedRecomp-NX's pattern): `futari_native.h` = Site (key, guest
  address, code hash), `ModeOf` (key + CPU master + FNV-1a code check, decided at the first call, logged), `Verify`
  (original and native from the same registers + declared memory ranges, ABI registers compared, the original's
  result kept; counted into `[verify]` as "natives: name checks/differences"). default.xex natives are hooks in the
  executable; core natives are compiled into each core target (`futari_native_core.cpp`, `FUTARI_CORE_<name>`), since
  a core's symbols are local to it. Tools: `tools/find_same_functions.py` (the copies of a function in all nine
  modules), `tools/native_code_hash.py` (the hash from `out/<module>_image.bin`).
  - `futari_native_memcpy`: the XDK memcpy is the same 290 instructions in all nine modules (default 8207DAC0,
    cores 88072260/88072270/88082260); non-overlapping copies become a host memcpy.

### CPU profile findings and fixes (2026-10-06, PC profiler)

The PC runs the game uncapped (the native renderer's present never blocks on the PC; ~1,800 frames a second in Xbox
360 mode, so scripted runs fast-forward through the gameplay): `futari_pc_swap_limit_hz = 60` holds each guest swap to
console pacing for test runs and profiles (off by default; the Switch is paced by its FIFO swapchain).

At console pacing, before the fixes: the game thread 96 % in D3D's GPU wait spin (real game work ~3 %), the ring thread
the bottleneck at uncapped rates. Fixed (each with its key in the enhancement table; verify mode 0 differences):

- `futari_native_repack_fast`: the k_32* vertex conversion (Futari's sprite shaders fetch signed integer coordinates;
  Vulkan has no 32-bit scaled formats, so the renderer converts each word to float) read the upload buffer BACK, and
  that memory has no CPU cache (NvMap on the Switch, write-combined on the PC): 91 % of the ring thread on the PC for
  ~465 bytes per draw. Now copied to a cached scratch buffer, converted there (one linear NEON pass for sprites, whose
  six k_32_32 attributes cover the whole 48-byte vertex) and written to the upload buffer once. Checked against the
  per-attribute loops for the first `futari_native_repack_verify` draws (every one in verify mode).
- `futari_ring_no_per_draw_copies`: three per-draw costs found with the profiler's caller attribution:
  `ProveRectangleList` copied the whole register file per rectangle list (now two words changed in place and restored);
  the "bounded draw extent" diagnostic built a fmt::format key on every draw until 32 distinct ones were seen (never,
  here); EDRAM4 depth-redirect declines and the EDRAM4 draw description were formatted per draw. Same logs.
- `futari_wait_blocking_ring`: Futari's D3D GPU wait (default.xex sub_820A4590, 32 db16cyc then a look at the
  read-pointer write-back word [[device+10896]] against [state+8]; state byte +10941 bit 0x02 = hung) is masseffect-nx's
  sub_8222FA98 at other offsets. If the read pointer has not moved, the game thread sleeps until the ring thread
  advances (NotifyRingProgress) or futari_wait_ring_max_us, then the original runs. Also hooked in the eight cores
  (88081310 / 88081300 / 88091300, same bytes). PC at 60 Hz: game thread 96 % -> ~8 % busy, 9.6 s of 10 asleep,
  still 60 swaps/s. On the PC the write-back word is in the physical views, which the PC keeps 0x1000 higher
  (REX_PHYS_HOST_OFFSET): host code reading guest memory must translate like the generated code.
- `futari_native_memcpy`: XDK memcpy in all nine modules (40 million verified calls, 0 differences).

Ring thread per draw on the PC (uncapped, Xbox 360 mode): ~71k draws/s -> ~365k draws/s; at 60 Hz the ring thread is
~13 % busy and the game thread ~8 %.

PC profiler additions: busy test (a quarter of a tick of cycles; suspension itself moves the counter), source lines of
the biggest functions, and callers (return addresses found on a copy of the stack top, right after a call
instruction, in futari*/rex* modules) with the call site's line.

### Boot hang from non-volatile guest loads (2026-10-06, console)

Builds B and C hung at boot on the console (0 swaps after the first 49 draws, game thread 100 %), the PC ran fine.
GCC -O3 hoisted guest loads out of polling loops once they were no longer volatile and compiled the loops as branches
to themselves: tools/find_self_loops.py counts 1 in the baseline ELF (a genuine guest halt in sub_82118438) and 172
in B. The one at boot: D3D's `while (device[11012] != 0) {}` in sub_820A9268. Direct calls let GCC inline callees, so
a callee's poll (the GPU wait's read pointer and clock) is hoisted into the caller's loop too. Fix:
tools/perf_codegen_post.py pass 3 puts REX_LOOP_BARRIER() (asm volatile memory clobber) at every loop header of the
generated code (~33,000 over nine modules); straight-line code keeps the plain loads. Check every Switch ELF with
find_self_loops.py before sending it.

### Config defaults and the sampler switch (2026-10-06, console run with an old futari.toml)

A console run showed "46 of 67 active": the SD card kept a futari.toml from an older build, so every newer key stayed
at its code default (mostly off), including futari's ring set and every fix of the day (game thread 96 %, ring
89 %, GPU idle 16-17 ms per frame). Apply() now sets a key the config does not mention (cvar source kDefault) to its
ON value while its group master is on. Exception, kExplicitOnly rows: futari_native_deferred_recording and its two
sub-keys. With futari_native_output_no_wait (on by default) the PC stalled ~100 ms per copy on GPU waits (23 fps);
either alone runs 60. They stay off unless set explicitly and the configs write them false (EXPERIMENTAL). The
CPU sampler no longer needs logs/rex/stacks_profile.flag: `switch_profile_stacks = true` (cpu_on/cpu_off/profile
configs) turns it on. PC with the new defaults, Xbox 360 mode at console pacing: 60 swaps/s, every self-check 0.

### Console-profile batches (2026-10-06, builds F and G)

From the first sampled console profile (build E): the game thread spun 78.6 % of its busy time in sub_8216B148's two
frame-pacing loops (`while (counter == last) {}` on 0x821F8560, bumped only by the vblank callback sub_8216AD38);
gameMainThr spent 27 % in the cores' LZSS decompressor; the ring thread's malloc/free (with newlib's lock) were ~14 %.
- F: `futari_frame_wait_sleep` ([[midasm_hook]] at 8216B244 / 8216B2E4 in app/perf_overrides_default.toml + a hook on
  sub_8216AD38; app/src/native/futari_native_frame.cpp), `futari_native_lzss` (futari_native_lzss.h; the game
  decompresses into physical allocations, so Host() translation, not a < 0xE0000000 guard), rectangle-list buffers
  reused, and the nfsmw driver's Spanish-named per-draw contract (nvk_switch_dibujo / vk_switch_precargar_pipeline).
- G: localized copies of the audio mixer's 8 hottest functions (tools/localize_native.py from SA2's tool;
  app/src/native/hot_functions.txt; headers in app/generated/hot). The guard (futari_native_hot.cpp) runs the copy in
  record mode (stores and vector accesses log the bytes they change; leaf callees localized too), rolls back the bytes
  that changed, replays the original and compares the whole context and every logged byte. A planted bug was caught on
  the first call. Shared notebook of all five ports: xbox360.md.

### The direct renderer, step 1 (2026-10-07, `futari_cpu_direct_state`)

sa2-nx's step 1 (its docs/direct-renderer.md), ported. The ring thread no longer parses the state packets of most draws:
right after Direct3D's draw function returns, the hook decodes the packets it wrote before its draw packet into a host
record and covers them with one NOP marker ('FTRD' + the record's number); the ring applies the record at the marker
with the same register functions (src/me/me_native_system.cpp, namespace direct).
- Futari's draws go through Direct3D's BeginVertices / BeginIndexedVertices (sprites; DrawVerticesUP calls them):
  default.xex sub_8209D800 / sub_8209DCE0 (src/me/futari_direct_hooks.cpp) and each core's own copy (8807ECA0 / 8807ECB0
  / 8808ECA0, src/native/futari_native_core.cpp through the exe exports FutariBeginDrawDirect / FutariEndDrawDirect).
  They write the draw packet but leave device+48 at the state's end; the draw packet's last word is at device+13652
  (EndVertices, sub_8209D7F0 / core sub_8807EC90, copies it to +48). A failed allocation (r3 = 0; BeginIndexedVertices:
  a nonzero HRESULT) leaves a stale +13652: no conversion.
- Device fields (XDK 2.0.9328, like SA2's): +48 write pointer (last word written, stwu), +52 / +56 segment limits.
- Not converted (stay PM4): D3D's Resolve (core sub_8807DD88, 961 instructions) and its quad helper (sub_8807C220):
  their DRAW_INDX_2 draws are the 13-18 % of ring draws left.
- Futari additions to SA2's design: records are held until XE_SWAP, so a predicated-tiling replay finds them again
  (0 replays so far); SET_CONSTANT2 / SET_SHADER_CONSTANTS carried; in verify mode every register run is applied by
  WriteConstantRunHost and by WriteConstantRun from the same register file and compared.
- PC verify: 1,234,855 records re-encoded identical to the packets, every marker found its record, 2,776,491 register
  runs equal to the PM4 path: 0 differences. Converted 98.8 % of hooked calls (the rest had a submission during the
  call), 82-87 % of ring draws.
- PC ring time: NO measurable gain. In-run A/B (`futari_direct_state_ab_s = 2`: on/off every 2 s in one run, the
  ring's work split by phase; banjo-nx's method): on 12.33 us per draw over 1.30 M draws, off 12.27 us over 1.32 M.
  (An earlier "1.85 -> 1.36 ms per frame" from two separate runs was noise: other ports' tests share this PC.) The PC
  profile agrees: PM4 parsing (Packet, Type3, register runs) was only ~8 % of the ring's busy samples; most of the
  ring is the Vulkan driver, DrawsVulkanImpl::Draw, the texture rechecks (XXH3) and memmove. The console, where SA2
  measured parsing at ~21 % of its ring, decides: configs/futari_test_direct_state_ab.toml.
- Round 2 (sasrt-nx's and banjo-nx's traps): records keep the region's first three words; the ring re-decodes the
  region when it applies a record (Direct3D patching packets after the call; banjo: SET_BIN_* masks patched after the
  draw) and writes the three words back afterwards (a rerun parses plain PM4); only the first drawing thread converts.
  Verify: 1,363,437 regions re-checked at apply time, 0 differences; no second drawing thread.
- `futari_ring_rectangle_fast`: the rectangle-list expansion writes a sprite's three corners once and doubles the
  block (6 copies instead of 18 small memcpy calls; the hottest line of Draw on the PC). Verify 809,836 / 0.

Lockstep checks (sa2-nx's method): SDK `vblank_per_swap` (one guest vblank per swap), `futari_video_frame` /
`futari_video_every` / `futari_video_hashes` (one '<game frame> <XXH3>' line per frame), `tools/compare_frames.py`, and
the input script in frames (`f<N>`). Futari is not fully deterministic even so: two cpu-off runs share ~700 of 3000
frames (title animations, some gameplay frames), so only labels that reproduce across runs of a config are compared.
- Direct state on vs off: 699 of 699 reproducible frames identical.
- Deferred recording: NOT pixel-identical. The Mode Select banner's fade differed on frames 3180-3184 (up to 65
  levels, different in each of two runs) while five runs without it agreed. It stays explicit-only.
- cpu-off presents one frame later than the defaults at the same label (a latency offset, not a pixel difference:
  frame 1700 captured raw is identical).

### Round 2 of the ring work (2026-10-07, the user's 50-60 % CPU goal; ARM first)

Measuring on a shared PC: wall time is useless (all five ports build and test at once). The in-run A/B
(`futari_ab_s` = 2, `futari_ab_key` = the enhancement: it alternates on/off every 2 s, and the ring's own work is
split by phase) counts the ring thread's CPU cycles on the PC (QueryThreadCycleTime) and nanoseconds on the Switch.
Supported keys: futari_cpu_direct_state, futari_ring_edram_memo, futari_ring_rectangle_fast,
futari_native_dynamic_state. Console configs: configs/futari_test_ab_direct_state.toml, futari_test_ab_edram_memo.toml.

Console baseline (build E profile, the last one; self time, pc lists are complete): ring 43.8 % of all busy CPU, main
thread 36.1 % (78 % of it the frame-wait spin, gone since F), audio mixer 7.9 %, gameMainThr 4.3 %, UI 3 %. Ring self
time: DrawsVulkanImpl::Draw 15 %, heap + mutexes 14.7 % (malloc/free/memalign mostly gone since F; mutexLock/Unlock
6.2 % still unexplained: the sampler now prints "pcl" pc/lr pairs and switch_hot_functions.py "callers of ..." so the
next console profile names them), PM4 parsing ~10 % (direct state), NVK ~8 %, XXH3 5.8 % (texture rechecks), EDRAM ~6 %.

- `futari_draw_census` (measurement): what changes between consecutive draws. 1.81 M draws: shaders 2.3 %, render
  state (2200-23FF) 20.7 %, PS constants 57.7 %, vertex fetch 84.3 %, texture fetch 37 %; only the vertices: 25.8 %.
- `futari_ring_edram_memo` (sa2-nx's EDRAM memo): 99.3 % of draws prepared from the memo; verify 1.57 M full-path
  comparisons, 0 differences. PC cycles A/B: about -1 % (EDRAM is ~5 % of the PC ring); console ~6 % of the ring.
- `futari_ring_rectangle_fast` part 2, the layout memo: consecutive sprite draws with the same source layout (compared
  by content) and VS keep the expanded rectangle layout (no entry copy, attribute/repack rebuild, XXH3 or set inserts
  per draw). Verify 1.36 M checks, 0 differences. PC cycles A/B (expansion + memo): about -6 % of the ring per draw.
- Direct state, PC cycles A/B: about +1 % (neutral): the console decides.
- Tried and dropped: constants dedupe (87 hits per ~25 k uploads); dynamic blend state (EDS1/2/3: 95.8 % of Futari's
  pipeline binds differ only in blend, ~160 binds a frame -> 7, ~8 % less PC ring work) is NOT pixel-identical in
  lockstep captures (212 of 1,098 reproduced gameplay frames differ; a third baseline differs on 7 of 1,107) although
  its own guard and verify mode pass. Open lead: fixing it would remove most of Futari's pipeline binds.
- Lockstep comparisons now use every frame and +-2 labels against several baselines (tools/compare_frames.py TEST
  BASE1 BASE2 ...): under PC load a frame can be presented one swap late.
- Round 3 (2026-10-07, early morning), work that always failed or was never used, removed exactly:
  - `futari_ring_pairing_skip`: Futari makes no Draw* records, so PairDraw's per-draw lookups (address table, replay
    map, queue) could only fail (console E: PairDraw 1.7 % of the ring). Skipped while the record counter is 0.
  - `futari_ring_dedupe_lean`: the vertex dedupe hashed every source for 0 hits (20,000 queries per 10 s); the
    fingerprint is no longer made for repacked bindings (it was never used for them), and the dedupe stops for the
    session after 100,000 queries with under 1 hit in 1,000 (the copy is the same bytes either way).
  - `futari_ring_extent_memo`: the extent estimator (a CPU interpretation of the VS) ran on nearly every draw and
    always rejected Futari's sprite shaders ("fetch-format", "partial-position-export"). Its microcode walk is a
    straight line whose decisions depend on the microcode only, so such a rejection holds until the next VS load
    (vs_identity_generation_): ~150-300 k estimator runs per 10 s skipped; verify 1.02 M re-checks, 0 differences.
  - Measurements added: "C6 draw stages" (DrawsVulkanImpl's stage stopwatch was collected but never printed;
    sasrt-nx's finding) with the vertex dedupe's hits; "C6 shared block census" (futari_draw_census): the shared block
    changes because of the texture slots alone (sprites alternate textures).
  - PC per-draw stages in gameplay: recording ~1.2 us, indices+vertices ~0.7, uploads ~0.55, pipeline ~0.5,
    textures 0.28 (spikes to 1.8 on texture rechecks), pass ~0.2.
- Build flags (2026-10-07): `-fno-math-errno -fno-trapping-math` on the generated game code (CMake option
  FUTARI_GENERATED_MATH_FLAGS, default ON; masseffect-nx ships them, sasrt-nx's reminder). IEEE results unchanged (no
  -ffinite-math-only). Lockstep: 426 of 427 reproducible menu frames and the 8 fade frames 3178-3185 identical to
  three baselines of the build without them; verify mode 0 differences.
- Lockstep caveat learned the hard way: Futari's gameplay frames (from ~2100) are only ~47 % reproducible even between
  two runs of the same build, and runs made back to back agree more than runs made at other times (PC load shifts
  the game's timing). Judge pixel identity on the reproducible stretches: the menus (1600-2100) and the Mode Select
  fade (3178-3185), where five runs without the change agree exactly. On those: dynamic blend state (EDS3) differs on
  fade frames 3179/3180/3182 (as deferred recording does), so both stay off; the math flags are identical.
- Measured with the in-run A/B against everything of today (futari_ab_key = futari_ring_all): about -10 % ring
  cycles per draw on the PC; the console number comes from configs/futari_test_ab_ring_all.toml.
- `futari_ring_sync_fences` (banjo-nx's finding): g_synchronizations_ring was never bumped by this ring, so the
  same-frame reuse by address (index cache over 16 KB, vertex dedupe's large ranges) was bounded only by the frame.
  Now every EVENT_WRITE_SHD / MEM_WRITE / INTERRUPT / WAIT_REG_MEM bumps it in stream order. Correctness; verify 0.
- sa2-nx's Switch settings adopted as rows: futari_native_fingerprint_mode = 2 (A57 CRC32 pair; PC falls back to
  XXH3) and futari_native_upload_memory = 1 (cached upload buffer, flushed before submit).

### The large stutter was the log (2026-10-07, console build M, cpu on)

- The user's run (out/console_logs/m_2026-10-07/on): frames of 1,022 ms and 277 ms (record 182.8 ms) at 12:36:07,
  then 335/250/440 ms at 12:38:16-18, etc. Every gameplay frame over 100 ms falls inside the ring's 10-second report
  (36 bursts of 1.0-2.1 s, from "GPU per Swap" to "C6 substages"). Over the run: 0.56 hitch frames/s inside the
  bursts against 0.07/s outside them, where the hitches are startup, loading and the module switch.
- rex_profile.log "during the hitches": the ring thread was in `fsdev_write` in 84-92 % of its samples, called through
  spdlog's rotating_file_sink -> fwrite -> `__sflush_r`. The game thread waits in WaitD3DGpuProgress for the ring.
  newlib's FILE buffer is 1 KB, so almost every long report line is an SD write of its own, made by the ring thread
  under the sink mutex. While the game streams BGM.xwb from the same SD card, one write took 50-150 ms (the
  "[io] SLOW" BGM reads went up to 61.7 ms at the same moments).
- Fix (SDK, sdk/src/core/logging.cpp, `log_file_writer`, default on): WriterFileSink. The logging thread formats
  the line and copies it into a memory buffer under the sink mutex (microseconds). A "Log writer" thread (0x3B on the
  Switch) writes the buffer once a second with one fwrite (64 KB FILE buffer), or sooner on 64 KB / warn+. Nothing
  blocks the logging thread. Lines past `log_file_writer_buffer_kb` (4 MB) are dropped and counted ("[log] N lines
  dropped"); that is the lesson of log_async's block policy. FlushLogging (crash handlers, exit) writes everything
  before returning. A Switch fatal error appends the unwritten lines to rex_crash.log (RexLogUnwrittenTail).
  Rotation works as in spdlog (base.N.log), per batch. PC: logs complete and in order (native run with reports, a
  trace-level run rotating at 1 MB). `log_file_writer = false` brings back the old sink.
- Other ports: the same logging.cpp is in every port's SDK copy and their periodic reports run on the ring thread as
  well.
- Warnings flush (flush_on warn), so frequent ones cost an SD write each (sasrt-nx's point). The same Futari run had
  304 warnings in 368 s, nearly all "[io] SLOW: read 131072 bytes of BGM.xwb" from the music stream's own thread: a
  128 KB read at 8-10 ms is the SD's normal speed. Both "[io] SLOW" lines are INFO now
  (sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp). Futari logs no "identity mismatch" lines (sasrt-nx's other demotion).

### The native Direct3D layer (2026-10-07 afternoon: removing the guest Direct3D, PM4 and EDRAM)

The user's order (relayed by sa2-nx, then confirmed): sa2-nx's API-level renderer (its docs/api-renderer.md) for Futari,
plus all of SA2's CPU optimisations and codegen improvements (gap list: docs/research/sa2_gap_list.md).

- **Census** (`futari_diag_d3d_census`, src/me/futari_d3d_census.cpp; census builds: tools/d3d_census_build.py writes
  out/census/ hooks for every Direct3D function of the nine modules, `cmake -DFUTARI_CENSUS_DIR=...`,
  `perf_codegen_post.py --hooked-list out/census/hooks.txt`; back to normal: the pass without the list and an empty
  FUTARI_CENSUS_DIR). PC, 100 s of menus + reco_normal gameplay, outermost PM4 writers: core BeginVertices 8807ECA0
  360k calls (32 words), default BeginVertices 8209D800 44k + 283k through DrawVerticesUP 8209DC98, core DrawVerticesUP
  8807F138 5.4k, Resolve 820A2280 9.3k / core 8807DD88 5.4k, Swap 820A5408 5.3k (+ 820A9268), Clear 820A19F0 1k,
  SetShaderGPRAllocation 8209B098 10k, the window scissor writer 82096B28 (via SetViewport / SetRenderTarget: 8209A100,
  820990E0, 820999A8; core 880753F8) 55k, texture coherency helpers (rare). Draw-time flags +10940..+10944 were
  00 04 04 00 00 at every draw (no predicated tiling).
- **Device layout** (XDK 2.0.9328): SA2's shadows and dirty masks at the same offsets up to +10900; moved: flags +10940,
  decl +11992, bound stride +12008, viz table +12298 / index +12363, stream stride +12704, PS +12868, VS +12872, draw
  end/data/words +13652/13656/13664, bin mask/select saves +12884/12888, 2000 base +13352, debug object +13712; the
  inline allocator's bookkeeping +15124..+15143 (and +10942 bit 4 when its heap wraps). Space function 820A8FC0, inline
  allocator 820A8138. Per-module globals: interpolator mask / flag / declaration ids, 12 bytes (default 0x821ED334).
- **Nine modules**: the native code is the executable's; each module describes itself with a `futari::api::Module` (its
  own __imp__ originals and globals; tools/d3d_module_table.py finds a core's copy by instructions with targets and
  immediates ignored); the cores call it through exe exports (futari_native_core.cpp).
- **Rare paths through Direct3D's own helper** (RedirectedCall): the shader load and the 0x2203/0x2000/0x2104 re-send run
  the original with +48/+52/+56 pointed at a scratch buffer; their packets become ops. Exact by construction (their
  device and shader-object updates are Direct3D's own), ~2 % of draws. The common flush and the draw are native.
- Pieces (each its own key, cvar only until verified): `futari_api_draws` (BeginVertices; DrawVerticesUP and EndVertices
  stay original: they call it / copy +13652 to +48), `futari_api_window_scissor`, `futari_api_ring_zero_ahead`. Records
  share the direct-state ring ('FTRA' markers: nothing under them to check or restore); verify mode's shadow runs as
  sa2-nx's (the inline allocator hooked to answer shadow runs from scratch).
- Found on the way: tools/perf_codegen_post.py counted addresses in comments, hot_functions.txt, record declarations and
  `__imp__sub_X` mentions as hooks (memset's 435 call sites kept their weak-alias call): fixed (sa2-nx had the same
  trap: 198 calls). Native memset (`futari_native_memset`, sa2-nx's n_827EFEA0, byte-identical in all nine modules).
- **PC verify** (110 s, menus + reco_normal): 699,410 native calls (BeginVertices in default.xex and the core, the
  window scissor writer) each compared with Direct3D's original run on a device copy: the ops equal the original's
  packets every time. Device-byte differences were only fields the ring thread writes concurrently (sa2-nx's trap 2):
  +10992..+10999 (the CP scratch registers' writeback; 94 hits) and +10904 (sa2-nx's +11032; 1 hit), now excluded with
  the allocator's own fields (+15124..+15143, +10942 bit 4). The ring applied every record ("not found 0"); no
  fallback. PC cost without verify: 0.44 us per native call (~1 % of a core; the console decides).
- **Pixels** (lockstep, frames 1600-3504): menus 431 of 431 judged frames identical. The other differences were
  animated elements on elapsed time, not rendering: the attract-mode ranking panels and "PRESS START" blink (frames
  2100+: the native-on images equal the native-off ones 3-7 labels later, the lag growing), and on the Mode Select fade
  (3170-3184) one background ripple at another position (raw frames compared: every other element, the timer "59"
  included, identical; the difference fades out at 3185). That fade spot is where deferred recording and EDS3 dynamic
  blend "differed" too: three unrelated timing changes, the same ripple. Their rejection may have been timing, not
  rendering (sa2-nx found deferred recording pixel-identical): worth re-checking with raw frames.
- Also verified with 0 differences in the same PC runs and on in every config: `futari_ring_target_memo` (sa2-nx's
  target memo: 573,128 checks), `futari_gpu_push_once` (518,878), `futari_native_memset` (120,637 + 1,091 in the core).
  The control run (native layer off, these on) matched three old baselines on 1,098 of 1,098 reproducible frames.
- The native layer's keys are rows of the table but explicit-only (off in the shipped configs) until the console run:
  configs/futari_test_api.toml (cpu_on + every futari_api_* key), configs/futari_test_api_verify.toml.
- **Native shader load** (`futari_api_shader_load`, ShaderLoadNative): Direct3D re-sends the pixel shader on nearly every
  draw (PC: the redirected shader load ran on 220,344 of 233,913 draws), so the "rare" path was the common one. Now
  native (sa2-nx's ShaderLoad with Futari's extras: the WAIT_UNTIL of +10943 bit 2, the 0x2001 update of bit 8);
  the inline copy, the binding match and the depth helper stay Direct3D's own; the debug-assert states run Direct3D's
  shader load through the scratch buffer.
- **Clear and Resolve through records** (`futari_api_clear`, `futari_api_resolve`, RedirectedEntry): Direct3D's own
  code with the command buffer on the scratch area; its packets become the record; no PM4 of theirs in the real
  command buffer. Traps found:
  - **The inline-data allocator lives in the command buffer** (sub_820A8138 -> sub_820A7F08): it places data after the
    write pointer or past the segment end and frees a block once the GPU's read pointer passes it (sub_820A7AD8). On
    the scratch area its positions were scratch-relative: Clear stalled the game (D3D GPU wait forever, ring idle);
    Resolve only survived because its allocations continued an existing region. Fix: the allocator runs on the real
    command buffer during a redirect (RedirectedInlineAlloc swaps +48/+52/+56 in), and the record's marker is written
    first so its data lands past it. (Likely what stalled sa2-nx's native Clear in Peggle 2.)
  - The space function is hooked: no submission while Direct3D code runs on the scratch area (a requested one is
    made on the real command buffer at its next space check). Nested native hooks pass through during a redirect.
  - A decoded packet that points into the scratch area stops the layer (checked: fetch constants, the copy
    destination, type-3 payloads except inline microcode / SET_CONSTANT / NOP; the first version flagged clear-shader
    microcode as an address).
- **PC verify, everything on** (110 s): 783,874 native calls (706,149 native shader loads, 974 Clears, 15,500
  Resolves, 61,251 window scissor) compared with the original on a device copy: 1 difference, +15168 (no guest code
  stores there: the ring's memory writes; excluded). **Pixels**: 1,098 of 1,099 reproducible frames identical (the
  Mode Select fade included); the other one (3051) is the title screen's ripple animation (raw frames: everything
  else identical).

### Vertex shaders Direct3D reorders (2026-10-07, `futari_gpu_vs_fetch_reorder`, sa2-nx's fix)

Direct3D's vertex fetch patcher (sub_8209EF58) rewrites a vertex shader's fetches for the vertex declaration and, for
some declarations, moves the declared fetches to each other's slots. The identity check holds each fetch at its own
slot, so such a load matched no package entry and its draws were dropped (sa2-nx: the Chao garden's stats window).
Futari showed none in the content played so far; ported at the user's request for later stages and modes.
- app/src/me/me_vertex_fetch_reorder.h (sa2-nx's, unchanged: ProveVertexFetchReorder, the offline repair tool's proof
  at run time, and its brute-force self-check); ShadersNative::IdentifyReordered / FetchReordered
  (futari_native_shaders.cpp); IdentifyShader tries it only when nothing else identified the load, and
  ComputeVertexIdentity accepts that one entry while the proof holds (me_native_system.cpp). The draw path's fetch
  selection already handles moved slots. Non-CPU row, on by default.
- Proof on Futari's own package (sa2-nx's shaders/tests/test_vertex_fetch_reorder.cpp --orders, built with a small
  unistd.h shim): 18 VS containers, every order placed, 0 wrong mappings; the 3-fetch shaders have a provable reorder,
  most 2-fetch ones swap fetches of different bases (refused, as Direct3D would not produce them).
- End to end with a test switch, `futari_diag_vs_reorder_swap` (not an enhancement): it swaps two fetches of a loaded
  vertex shader where the proof allows, i.e. makes the loads Direct3D makes for such declarations. Fix OFF: 201,597
  to 247,740 draws per 10 s without shaders (dropped). Fix ON: 0 dropped, every frame of the menus (which draw with the
  swapped shader) identical in lockstep, 48 of 48 raw attract frames identical; the self-check against brute force
  0 differences.

### GPU round (2026-10-08: sa2-nx's GPU optimisations, the user's request "port as many as you can")

The console (build P, cpu on, 59-60 fps) spends per Swap: the 640-wide sprite target 4-8 ms, copies 0.8-1.5 ms,
scene_no_z ~1.5 ms, the rest idle (gap 5-10 ms). Every item below is sa2-nx's, verified pixel-identical there; each
has its own key (non-CPU group, on by default) and was checked here again (below).

**Shader package.** XenosRecomp (`shaders/XenosRecomp/shader_recompiler.cpp`, `shader_common.h`) is now sa2-nx's: the
same files plus its GPU-round additions (Futari had none of its own changes; key names renamed in comments). Rebuilt
with the same 61 containers (`tools/build_shader_library.sh out/shaders_prev/in`; the old translator reproduces the old
package byte for byte, kept in out/shaders_prev). Always on in the package (no key): `futariApplyTextureSigns`
returns at once when the packed signs are 0, and `remapInput` uses select chains instead of a dynamically indexed
vector (local memory on Maxwell). Of the variants: 28 of 31 VS have the index-unused form, 30 of 30 PS the blend-skip
form, 0 shaders the structured control flow (none of Futari's uses the skipTo form: sa2_gpu_structured_cf has nothing
to do and is not ported).

| Key | What | Futari use (PC, per 10 s) |
|---|---|---|
| `futari_gpu_blend_skip` | the PS discards a pixel whose blend would leave the RGBA8 target unchanged (alpha <= 0 with SRC_ALPHA/ONE_MINUS_SRC_ALPHA or additive, all-zero source with ONE/ONE_MINUS_SRC_ALPHA); only RGBA8 UNORM without bias (Futari also checks the pass's real attachment format), no depth/stencil write, no alpha to coverage, op ADD; NaN never discarded; occlusion queries are answered on the CPU | 95-99 % of draws |
| `futari_gpu_texcoords_unswapped` | VS skip the swapped-texcoord test (g_SwappedTexcoords is always 0) | 100 % |
| `futari_gpu_no_texture_signs` | PS skip the per-fetch sign test when no texture of the draw has signs | 100 % |
| `futari_gpu_identity_remap` | VS skip the per-input remap test when every input keeps its order | 100 % |
| `futari_gpu_index_base_held` | VS proven not to read r0's start: r0 = 0 and g_VertexIndexBase held (not rect lists) | 0 %: Futari draws rectangle lists |
| `futari_gpu_pass_narrow_end` | render pass end dependency names its consumers (no NVK constant/QMD/MME cache invalidation per pass end) | every pass |
| `futari_gpu_query_reset_written` | each submission resets only the timestamp queries written, not 8192 | every submission |
| `futari_gpu_clear_merge` | a redirected EDRAM clear (Direct3D's Clear through the 4x alias, into the 1x color owners) clears all its regions between ONE barrier pair, rows merged into rectangles | 45 regions -> 1 rectangle per clear, one clear a frame: ~44 clear passes and ~88 GPU wait-for-idles less per frame |
| `futari_gpu_resolve_draw` | same-format RGBA8 resolves as a fragment pass on the 3D engine (`shaders/futari_resolve_copy.frag`) instead of vkCmdCopyImage on NVK's copy engine (engine switch + wait for idle each) | every resolve (the 1280x720 frame and the 640x480 ones), 0 declined |
| `futari_gpu_prewarm_current_keys` | the prewarm skips list records whose GPU-round bits this session cannot produce (records of older builds lack the texcoords bit) | start-up only |
| `futari_gpu_linear_fetch_not_resolved` | fidelity fix (sa2-nx's Wild Canyon): a linear fetch is never taken for a resolved texture | counted; 0 so far |

Not ported, with the reason: sa2_gpu_structured_cf (no skipTo shaders), sa2_gpu_resolve_alias (every Futari resolve
has draws before it: no duplicate resolves), sa2_native_edram4_clear_color0 and the 2048 conversion budget (Futari has
no EDRAM conversions or rollovers), sa2_native_zcull_1x_depth (off in sa2-nx too; Futari's depth time is 0),
sa2_gpu_msaa / render_scale (change the image), sa2_native_constants_dedupe (tried and dropped here: 87 hits per 25k),
sa2_native_dedupe_sync_epochs (Futari has futari_ring_sync_fences), sa2_vsync_relaxed (pacing, not GPU time).

**Verification (PC, lockstep, board 1_0 frames 1200-4199 and Black Label Xbox 360 frames 2700-5699).** Two control runs
of the old package with the new keys off; a frame is judged where they agree (1,186 labels on 1_0: menus, Mode Select,
Character Select; 423 on Black Label) and is identical when the test's image is either control's within +-2 labels
(`tools/compare_frames.py --any`, new: two controls are often a frame apart on wall-clock animations and a test can
follow either). Runs: the new package with every key off; all keys on; each key alone (10 runs); verify mode.
- Every run identical on every judged label except three stretch edges, the same in the keys-off run: 3073-3079 (the
  next screen forming in the white transition panel), 3176-3185 (the Mode Select banner fade, already known as
  timing), 3604-3611 (Character Select's falling light particles). Raw full-resolution frames: the differing pixels
  are those animations a frame out of phase (the control changes the same pixels between its own frames) and both
  runs converge to bit-identical frames when the animation ends (3186, 3612). Resolve draw alone: 1,186 of 1,186.
- Black Label: all judged frames identical but the attract ranking screen (2700-2757, every third frame, elapsed-time
  animation); raw frames 2700-2711 of a control and an all-on run: bit-identical.
- Verify mode (futari_cpu_verify, all keys on): "GPU specializations" 2,948,937 checks, 0 differences (shared[65] = 0,
  no texture signs in any slot the bit was set for, every remap code 0xFFF, index base 0); every other fast path 0.
- Coverage per 10 s: blend skip 95-99 % of draws, texcoords / signs / remap 100 %, index base held 0 % (rectangle
  lists), 26,820 redirected-clear regions cleared as 596 rectangles, ~600-1,570 resolves drawn, 0 declined; linear
  fetches at a resolved address 0.
- Console: after the new shader package the pipelines with the new bits are created once on first use (68-159 ms
  each, Futari has ~12), then come from the cache and the prewarm list; futari_gpu_prewarm_current_keys skips the old
  list's records without the texcoords bit.

### Achievement toasts in Japanese on the Switch (2026-10-08)

The SDK's achievement toast (sdk/src/ui/overlay/achievement_toast.cpp) shows the title's own strings: Futari's are
Japanese (UTF-8 in the title's XDBF string table, language 2; 50 achievements). The ImGui atlas had ProggyTiny (Latin)
plus, on Windows only, MS Gothic: on the Switch every Japanese glyph fell back to '?'. Now the Switch build merges the
console's shared Standard font (pl:u, PlSharedFontType_Standard: Latin + Japanese) the same way, 12 px with ImGui's
Japanese ranges (all 342 distinct characters of Futari's Japanese string table are in them). The font is copied out
of pl:u's shared memory and decoded when it comes as BFTTF (key derived from the known sfnt header, hitmango_nx's
method): sdk/src/ui/switch_shared_font.cpp, called from imgui_drawer.cpp. Log line: "ImGui: the Switch's shared
Standard font merged for Japanese text".

### Black Label Character Select garbage and the audio pops (2026-10-08 evening)

**The glitch** (console only, after playing Ver1.5 Xbox 360 then Black Label Xbox 360 Maniac: the Character Select
portrait and background as magenta/black stripes). The console log of the glitch run: Ver1.5's menus resolve six
512x512 textures at 0DF85000-0E205000; at Black Label's Character Select, "partial resolve: advanced base 0E208000
belongs to 0E205000; local destination 192,0". SearchResolvedPartial (banjo-nx's, for Direct3D's predicated tiling,
which resolves one texture as several rectangles with an advanced destination base) took Black Label's own resolve
for a later rectangle of the stale Ver1.5 texture, wrote it there, and made no texture at 0E208000: Black Label's
draws then read guest memory at that address. It depends on the session's heap layout (the PC runs, with the user's
save and replays, put the 512x512 textures elsewhere and never reproduced it). Fix: futari_gpu_partial_resolve_fresh,
a parent must have been resolved since the last Present (Resolved::presented_written); a skipped stale parent is
logged once per pair. PC lockstep unchanged. To be confirmed on the console.

**Stutter** (console build Q, deferred recording off): one 10 s window at ~40 fps with ~1,000 draws a frame; the
ring cost 16-18 us per draw without deferred recording and 5-6 us with it (user's earlier run, clean): deferred
recording is back in futari_all_on.toml / futari_cpu_on.toml.

**Audio pops.** The Switch output never ran dry nor clipped (driver counters), so they are made before the driver. A
PC dump (audio_dump_s, Black Label Maniac) shows 8 steps exactly on 256-sample frame edges in the first 10 s of play
(both channels jump together, then continue: a voice cut or started between two frames), no repeated or dropped
frames. The music (BGM.xwb) is 48 kHz stereo PCM streamed in 128 KB reads every 0.68 s; a few took 28-97 ms on the
console. New SDK diagnostic audio_pop_detect (on): "[audio-pop]" lines for edge steps, dropouts and late frames.
Console run with the detector (Black Label Maniac): steps all through play, and at the end one effect cut at -0.14
six times every 0.15 s. The user's replay (slot 6) on the PC gives the same kind of steps with the native hot audio
functions on (13) and off (6, three at the same moments), and no XMA context is ever made (the effects and the
streamed music are the game's own PCM): the cuts are the game's own mixer, as on the 360. audio_switch_declick (on,
Switch driver) fades such a step out over 64 samples (the detector's rule: > 0.05 and > 1.5x the largest jump inside
both frames); simulated on the replay's audio it touches 13 edges, 17 ms in 170 s.

### Crackles in Xbox 360 mode (2026-10-08 night)

The user heard crackles in Ver1.5 Xbox 360 mode (on the PGO-instrumented build) and saved the run (replay slot 5,
"new 5" save; the user recalls the same crackles in Xenia). Played on the PC with audio_dump_s (scratchpad
replay_audio.ps1: board:0:1, REPLAY DATA, slot 5; saves in D:\mushihemesama\userdata-replays) and analysed:
- the game renders plain stereo (C, LFE and surrounds silent), raw peaks up to 1.44; the Switch fold (x 0.586) peaks at
  0.84: no clipping; no non-finite or denormal samples; above 16 kHz at most 1.4 % of the energy (no aliasing);
- 2 one-frame gaps in 8 minutes of play (223.2 and 391.0 s); the 4-per-press "dropouts" in the replay menu are inside
  a near-silent cursor effect;
- 137 steps on 256-sample frame edges, in bursts: at 535-540 s a sound is retriggered every 0.064-0.070 s (4 game
  frames) and cuts its previous instance each time, 15 steps a second (37 in 10 s): the crackle. The same steps repeat
  sample for sample when the same passage plays again (215 s and 497 s), so they are in what the game delivers.
- The console logs of earlier sessions (Xbox 360 mode included) never ran dry and never saturated, so it is not the
  output; the instrumented build is slower, which can only add underruns.
The 1.3 ms linear declick (S onward) caught 135 of those steps but left a tick and added low-mid energy (HP > 200 Hz
in the 2 ms after the edge: 1.03 -> 1.08 of the frame's own level). sdk/src/audio/declick.h (portable; the Switch
driver and tools/declick_test.cpp run the same code): audio_switch_declick_smooth (on: the step comes back over the
whole frame, 5.3 ms, raised cosine: 0.98) and audio_switch_declick_predict (on: also a step against the last two
samples' slope, > 6x the frames' mean |second difference| and > 0.03: 339 edges in the replay instead of 135, 0.24 a
second; 45 instead of 37 in the burst). Every other sample is unchanged (checked on the burst: only the treated frames
differ). out/audio_compare/burst_535s_{off,v1,smooth,predict,both}.wav are the burst with each setting.

### The installer and its shader library (2026-10-08 night)

futari-nx-installer (sa2-nx-installer's design) makes the SD folder in the browser from the player's disc image: the
XDVDFS reader (`lib/xdvdfs.js`; full dumps with the game partition at 0xFD90000 or rebuilt images at 0), the DLC as
package files (STFS header: title 435A07D7, content type 2) or unpacked folders, and the shader library made from the
nine executables with the same steps as the tools here, in JavaScript and WebAssembly.
- **55 shaders, not 61.** The static recipe (42 containers + 13 wrapped raw programs; `tools/extract_shaders.py`,
  `sweep_raw_shaders.py`, `wrap_discovered.py`) reproduces 53 of the 61 inputs of the library tested until then; the
  other 8 were run-time leftovers (6 per-declaration vertex variants the renderer no longer needs since the identity
  second pass, and 2 older wraps of shaders the recipe also covers). The JavaScript recipe gives the same 55 containers
  byte for byte. The 55-shader library (native DXC) on the PC: lockstep like a baseline (3 / 12 differences against 3
  baselines, a pure baseline 14), 0 unidentified loads and 0 missing shaders, also over the user's two replays (slot 5
  Ver1.5 Xbox 360, slot 6 Black Label). The only cost: 18 pipelines of the old variants are rebuilt once.
- **The browser's DXC broke the sprites.** The installer's DXC is the web build of v2025.1 (nfsmw-nx-installer's);
  the native one is 1.8.2407. The translator's HLSL is identical (55/55), the SPIR-V differs for 17 shaders, and the
  page's library drew the menus garbled (866k of 921k pixels different at the same lockstep frame). Bisected with
  hybrid libraries to the vertex shaders of rectangle-list sprites: v2025.1 reads input components straight from the
  Input variable (`OpAccessChain %in.var.POSITION0 0`) where 1.8.2407 copied every input into a function variable
  first, and `ExpandRectangleShader` (`me_rectangle_spirv.h`), which turns the inputs into Private variables to build
  the fourth corner, declined any access chain into an input: those rectangles were not expanded. The transform now
  retypes such a chain to a Private pointer (pointer arithmetic into an input is still declined). With it the page's
  library draws the same frames as the native one, pixel for pixel; shaders from 1.8.2407 take the old path unchanged.
  The release library is the page's (its SHA-256 is in the installer's manifest), so the NRO is tested with it.

### Release setup (2026-10-08 evening: 1.0.0)

- **Settings built in** (sa2-nx's design): app/futari_switch_defaults.toml (written by tools/make_enhancement_configs.py:
  every enhancement at its current value, the experimental ones included; logging and profiling off) is compiled into
  the NRO (app/CMakeLists.txt configure_file -> futari_builtin_config.h, FutariApp::BuiltInConfig) and applied before
  futari.toml (SDK: rex::cvar::LoadConfigString, ReXApp::BuiltInConfig / DefaultConfigFile). futari.toml is the
  player's file (app/futari_switch.toml: the input options), written next to the NRO when missing. The configs/ folder
  still holds test configs; they turn logging back on (log_to_file, switch_profile_report).
- **No logs**: log_level = "off", log_to_file = false (SDK: no log file, stderr not redirected), switch_profile_report
  = false (no rex_profile.log, no sampling), futari_shader_misses = "", audio_pop_detect = false. logs/ and logs/rex/
  are created only when something is written (RexSwitchEnsureLogDir; the crash and assert logs call it).
- **Input options** (sa2-nx's SDK switch_input_driver.cpp, input_system.cpp, input_driver.h): input_xbox_layout,
  input_joycon_sideways, [input_p1] / [input_p2] button maps (a = "B, ZR"; flattened to input_p1_a).
- **Rename**: every masseffect identifier, cvar, macro, file and folder is futari now (masseffect::native ->
  futari::renderer, since futari::native existed; app/src/me/masseffect -> app/src/me/futari; masseffect_* cvars ->
  futari_*; SDK masseffect_io_* -> futari_io_*). "masseffect-nx" (the other port's name) and comments about Mass Effect
  itself stay. The shader package was rebuilt (only the shaders' debug names changed). PC lockstep unchanged.
- **NRO**: "Mushihimesama Futari", author ChanseyIsTheBest, version 1.0.0, icon app/futari_icon.jpg (256x256 from the
  cover: the logo and Palm's and Reco's faces).
- **Compiler** (sa2-nx's set): LTO (-flto, -flto-partition=balanced, 2 LTRANS jobs) and -fipa-pta for futari_recomp
  and the app; the eight game cores stay out (each is partially linked and its names made local with objcopy, which
  needs machine code, not LTO's intermediate code; per-core LTO at the ld -r step is possible later); hot functions
  first (app/hot_functions.ld from tools/hot_order.py: three console profiles, builds P/Q/R, old renderer names
  translated); -g1 line tables for the app; XXH_FORCE_MEMORY_ACCESS=0. tools/check_switch_build.py: _start at 0,
  Mesa string, no vector fmla in recompiled code.
- **PGO** (sa2-nx's design): build_switch.ps1 -Pgo generate -BuildDir out\switch_pgo makes the instrumented NRO
  (app/src/futari_pgo.cpp: pgo/futari_pgo_<N>.stream next to the NRO, every 2 minutes, on HOME and at exit);
  tools/win/pgo_merge.ps1 merges the streams into pgo/; build_switch.ps1 -Pgo use builds from them.
  First PGO release (V, 2026-10-08 night): one session of the user's (66 objects); GCC marked 626 functions hot
  (.text.hot.NAME) and 604 never run (.text.unlikely.NAME). The hot order's patterns missed the renamed hot ones (only
  1228 of the list in the ordered block, the rest after the unlikely block): app/hot_functions.ld and hot_order.py now
  match .text.hot.NAME too (1445 of 1467 names in the first 2.41 MB, then the unlikely block).
- **Profiler off by default** (the user's request, after T): switch_profile_report now defaults to false in the code too
  (the test configs set it true), and the new futari_gpu_timestamps (kInitOnly, built-in false) skips the timestamp
  query pool, so the release writes no GPU timestamps at all (with futari_gpu_marks_categories = false it still wrote
  the first and last of every submission, read back after each one, for the log's "GPU per Swap"). Every use of the
  pool was already guarded by queries_ != VK_NULL_HANDLE. The profiler thread itself stays: with the report off it only
  wakes every 100 ms to publish FPS and resolution to the overlay (no sampling, no file).
- **PC build gotcha**: run `cmake --build out\pc` from PowerShell after `. tools\win\env.ps1` whenever a reconfigure may
  happen (a changed CMakeLists or a CONFIGURE_DEPENDS file such as the built-in tomls). From the bash shell the
  reconfigure runs without clang on the PATH and without PROCESSOR_ARCHITECTURE: it cached an empty processor
  ("Unsupported Windows architecture") and msys's ninja. Repair: delete out\pc\CMakeFiles\<cmake version> and
  `cmake --preset win-pc -DCMAKE_MAKE_PROGRAM=<VS ninja>` from app\ in that PowerShell.

### Next steps (left for the next session, 2026-10-07 morning)

- The console numbers: build M with configs/futari_test_ab_ring_all.toml (all of today's ring work on/off every 2 s;
  "[native] A/B of futari_ring_all" lines) and the default config's rex_profile.log, now with "pcl" pc/lr pairs:
  `python tools/switch_hot_functions.py rex_profile.log --map out/switch_ab/M_mathflags/futari.map` prints "callers of"
  for mutexLock/malloc (build E: 6.2 % of the ring in mutexLock/Unlock, still unattributed).
- Codegen phase B2 TRIED 2026-10-07 06:00 and REVERTED: with `non_volatile_as_local` on and the scan's marks
  (docs/research/b2_marks/: 72 for default.xex, 24 per core) the PC build crashed 1-2 s into boot on every board
  ("Unhandled guest access violation: read of guest 0x1337F001" in default.xex sub_821186A8, a load through
  r28 = r3 + 28: entered with a garbage r3). sub_821186A8 has no direct caller (function pointer / vtable). Likely: a
  share_registers reader reached through REX_CALL_INDIRECT_FUNC from an unmarked caller, whose r14-r31 stay in locals
  (the codegen syncs locals into ctx only around DIRECT calls to share_registers functions). Needed first: a codegen
  sync of non-volatiles around indirect calls (or marking every function that makes indirect calls), then the
  localized hot copies regenerated from the new code (tools/localize_native.py; they then match the originals).
  Also (sasrt-nx / banjo-nx, 2026-10-07): the read-before-write scanners missed vector-register reads (every vector
  op is emitted as simde_mm_store_*(vD, expr) and was counted as a memory store), so Futari's marks may be incomplete
  for v14-v31; sasrt-nx keeps vectors in ctx (SDK option non_volatile_vector_as_local = false). Re-scan with that fix
  before the next B2 attempt.
- Codegen phase B2 (`non_volatile_as_local`, `non_argument_as_local`; masseffect-nx +9 %, sasrt-nx and banjo-nx
  shipped it 2026-10-07 with 0 verify differences): the hazard list is ready (`tools/scan_caller_register_reads.py
  --toml --with-callers`: default 35 readers + 34 callers, each core 13 + 11; the per-core marks need a
  perf_overrides_<core>.toml include). Two things to settle first: (1) the hot-native guard compares the whole
  context, and with non_argument_as_local the original leaves r0/r2/r11/r12 stale in ctx (false DIFFERENCEs): compare
  the ABI-visible registers only, or regenerate the localized headers from the new code; (2) [functions] entries at
  __savegprlr/__restgprlr addresses cancel the helper elision (sasrt-nx).
- Vertex copies across frames (sasrt-nx's measurement: 98-100 % of the vertex bytes still copied come from pages
  unchanged since their last copy): a cross-frame cache needs exact write tracking of guest memory.
- Fixing deferred recording's fade difference (bisection: futari_deferred_native_update is the prime suspect) and
  EDS3's (both differ on the Mode Select fade) would remove ~95 % of Futari's pipeline binds (EDS3) or move the Vulkan
  recording off the ring (deferred): both off until a fade lockstep check passes.
- From the other ports, for the next session: sa2-nx's `fp_moves_modeless` generator option (FPCR writes around scalar
  float moves in VMX code were 20-26 % of SA2's console game-thread samples: count samples right after `msr fpcr`,
  instruction words 0xD51B44xx, in Futari's audio mixer); sa2-nx's ring-side lockstep (the ring waits after each
  presented swap until the recorder has it; labels = presented swaps) is fully reproducible in SA2, unlike the
  time-sampled recorder here (~47 % of gameplay frames); futari_native_upload_memory = 1 is Switch-only (the
  table row is under REX_PLATFORM_SWITCH: banjo-nx's PC ring got 2.5-3x slower per draw with it);
  futari_native_fingerprints_sampling tripped in 56/58 SA2 PC runs (never in Futari's: no "SWITCHED OFF").
- sa2-nx's direct renderer step 2 (2026-10-07, `sa2_cpu_direct_flush`; its docs/direct-renderer.md "Step 2"): Direct3D's
  three flush helpers (constants, register groups, fetch constants) write the record's ops straight from the device's
  shadow and leave a 3-word placeholder NOP, instead of PM4 that EndDrawDirect decodes back. In Futari the helpers
  are called from BeginVertices (core sub_8807A578 / sub_8807A910 and their copies; default.xex sub_8209EA70 and
  others); records are held until XE_SWAP here, so a placeholder turned into a marker must stay valid until then.
  Not started: the console game thread's D3D share was small (~1.5 % of a core in build E), and the change needs its
  self-check (run the original on a device copy) before it can be trusted. SA2's other leads: resolves drawn on the 3D
  engine, blend skip, duplicate resolves as aliases; "nothing drew in between" must check edram4_version_ (redirected
  clears are not counted by Drawn()). Futari has 0 rejected draws (no cause 109 from the linear-fetch bug).
