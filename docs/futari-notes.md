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

### The crackle's cause, and the fade on cut (2026-10-26, 1.0.3)

The user: no crackle on a real 360, so an emulation difference was looked for. Every test is one PC replay of slot 5
(lockstep, declick keys off), counted with the detector's rule (135 steps over the run, 31 in the 535-545 s burst):
- Not the VMX float semantics: the five codegen options of sdk/include/rex/codegen/config.h (vmx_exact_minmax,
  vmx_fused_madd, vmx_expte_range, vmx_cmpb_nan, vmx_dot_overflow_qnan, all on in app/perf_overrides.toml): 135 / 31.
- Not the audio pacing: audio_sdl_pump (the callback every 5.333 ms, as the 360's hardware calls it): 136. The game's
  render-driver callback (sub_821396C0) wakes XAudio2's thread and waits for its frame; XAudio2 waits with no timeout
  (engine +124 = 1) and runs exactly one pass per callback (no frame lost or mixed twice).
- Not the update rate: the game's sound thread (default sub_821CB108) runs its callback (the game core's
  sub_88090480: XACT's update, its 8 channel fades every 16 loops) and waits 1 ms; Windows' 15.6 ms tick makes that
  ~16 ms. With timeBeginPeriod(1) it ran ~4x as often: 121 steps. Also not the clock (mftb is the 50 MHz guest clock),
  the spinlocks (real locks) or XACT instance limits (every cue is simple, every category limit 255, no fades).
- The cause (app/src/native/futari_audio_diag.cpp: XAudio2 / XACT calls with their threads, callers and the voices'
  resampler output): the game plays each effect on one of its channels, and playing on a channel FIRST destroys the
  cue still playing there (reco_abnormal sub_88090588 -> sub_880901B8: UnRegisterNotification, IXACT3Cue::Destroy;
  14,997 of 15,005 plays did, up to 23 plays a second of se11a in the burst). XACT's Destroy (default sub_820EC8F8)
  deletes the cue at once and XAudio2 drops the voice from the next pass. The effects (SE.xwb: 70 mono 44.1 kHz xWMA
  waves, decoded in software by XACT) are near full scale where they are cut: an ffmpeg decode of the bank's own
  packets (scratchpad xwb_to_xwma.py) peaks 0.4-1.0 in every 10 ms of the first 150 ms. XAudio2 has no click-free stop
  or destroy, so the same calls cut the same way whatever runs them; whether a real 360 hides it is not known.

The fade (app/src/native/futari_sound_fade.cpp; tested as the key futari_sound_fade_on_cut, always on without a key
since 1.0.3 at the user's request): IXACT3Cue::Destroy first sets the
cue's output matrix to zeros (IXACT3Cue::SetMatrixCoefficients, sub_820ECE08, with the counts of the game's own calls:
2 x 6) and returns S_OK; the cue is destroyed 4 XAudio2 passes (21 ms) later, from the game's next Play or Destroy.
Pending cues are destroyed at once before IXACT3SoundBank::Destroy (sub_820EEB60) and IXACT3Engine::ShutDown
(sub_820E6A98, vtable 0x82013F48 slot 5). The pass count comes from futari_audio_diag.cpp's hook of sub_82137218 (the
Switch build keeps only that part of the file). Replay: 1 step in 600 s (a one-channel attack at 253.5 s that a run
without the fade had too) instead of 135, 0 in the burst, the burst's level unchanged (RMS 0.133 / 0.132), dropouts and
late frames as before (3 one-channel steps in the next run, VMX options off). out/audio_compare/fade_burst_535s_off.wav
is the burst. Console (test NRO out/switch_ab/AF_sound_fade_test, declick off): the user heard one crackle in a whole
run, and the fade sounds better than the declick. 1.0.3 forces the Switch declick (above) off whatever a config says
(futari_enhancements.cpp Apply; its three keys left the enhancement table); neither one adds latency (the declick
only reshapes the frame after a step, the fade only lets the cut sound last 21 ms longer). The VMX options are off
again in 1.0.3 (app/generated restored from out/generated_before_vmx).

### Two players (2026-10-14)

Tested on the PC after peggle2-nx found that its game needs a second signed-in user for player 2: Futari does not.
With two scripted pads (`p2:` steps: `72:p2:start,...,82-120:p2:a`) in Ver 1.5 Xbox 360 mode, player 2 joins during
play: its own score and counter on the right and a second ship. The game imports XamShowSigninUI but never called it
(0 calls in the log); no SDK change is needed. Not tested on the console (the controller assignment there is the SDK's
Switch input driver, sa2-nx's).

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
  **That repair was not complete (found 2026-10-14).** The broken configure had also cached the C++ per-configuration
  flags as empty (`CMAKE_CXX_FLAGS_RELWITHDEBINFO=`, where a fresh tree has `-O2 -DNDEBUG`; the C ones were right), so
  every PC executable from 2026-10-08 to 2026-10-14 was built without optimisation: 27 MB instead of 12, and slow
  enough to make the game skip more frames, which is why the lockstep runs "got noisier after the full rebuild".
  Results of those runs stand (same sources, same arithmetic), their timings do not. Found by comparing
  `CMakeCache.txt` with the fresh 1.0.0 reference tree's; repaired by setting the four `CMAKE_CXX_FLAGS_<CONFIG>`
  back. After any cache accident: `grep -c -- -O2 out/pc/build.ninja` (1,236 in a sound tree, 497 in the damaged one).

### Long sessions: the texture view slots (2026-10-14, `futari_native_texture_slots_evict`; sa2-nx's finding)

sa2-nx's players reported black textures and missing geometry after three to four hours; its cause was the 2D texture
view heap of this renderer: 4,096 descriptor slots (`kCapacityHeap = {4096, 16, 64, 512}`), and `ReserveSlot` answered
0 when they were used up, which is an empty texture from then on. The texture cache evicts by bytes and by age of use,
not by slots, so a session that keeps loading new textures faster than old ones age out runs into it. Futari has the
same code and no report yet.
- **What:** a heap more than 3/4 full sheds its coldest textures (unused for 120 frames, not being uploaded) in
  batches of 32 every 16 frames until it is under 5/8; a heap that did refuse a slot drops an eighth of its capacity
  at the next Swap. `[native] C3 texture slots (...)` every 600 frames says how full each heap is, how many textures
  and megabytes the cache holds, how many were dropped for slots and how many views found no slot.
- **Exact:** a dropped texture is created again from guest memory at its next use, as after the cache's own eviction.
- **Tests (PC):** `futari_diag_texture_slots` limits the 2D heap. About 91 to 100 slots are render-target views the
  texture cache does not own, so a limit of 100 cannot be met by any eviction (that test only showed the limit is
  enforced). With 300: 483 textures dropped for slots, 0 views without a slot, lockstep 532 of 541 labels identical
  (the rest the known timing labels); with the key off at the same limit 1,559 views found no slot. Default limit:
  nothing is dropped (506 of 513).
- **Not ported:** sa2-nx's release of idle resolved textures (`sa2_native_resolved_retire_idle`). Its console answer
  was that it did not cure the reports, and Futari's resolved-image count stays flat (93 to 99 in every PC run,
  the two replays included): the game keeps its render targets for the whole session.

### The CPU rounds of 1.0.1 (2026-10-14: sa2-nx's three rounds, on nine modules)

The user's request after 1.0.0: "may as well do all the codegen improvements as well and the other adventure 2 fixes
that are relevant". sa2-nx 1.0.1 carries three rounds of CPU work on the generated code (its `docs/cpu-rounds.md`;
the first is sasrt-nx's). All of it is build-time: the Switch build gets fewer instructions for the same statements,
and the PC build keeps the old operation next to each new one and compares them while it runs.

**The SDK** is at sa2-nx 1.0.1's level (its commits f104e18..8be5b45 merged three ways; the generator's files taken
whole, `project_recompiler.cpp` with Futari's DLL handling kept). Regenerating with unchanged options gave the 1.0.0
text again (stamps and one new macro aside), so the merge by itself changed nothing. `sdk/tests` are sa2-nx's ten
(`tools\win\run_sdk_tests.ps1`: 0 failed; the conversions 17.4 billion checks, the any-mode float helpers 600 million).

**Two corrections of 1.0.0** (both from the other ports' audits):
- `fctid` / `fctiw` rounded halves away from zero on the Switch: simde's portable `cvtsd_si64/32` is `round()`
  there (`fcvtas`), where the 360 and x86 round in the current mode (to even). 26 `fcvtas` in 14 recompiled
  functions of the 1.0.0 ELF, two of them the hot float-to-integer helpers n_8207AE60 / n_8207AF40. The generator
  option `fcti_inline` writes `rex::ppc::fctid` (`frinti` + `fcvtzs`). `tools/check_switch_build.py` now fails on
  any `fcvtas` in recompiled code.
- `-fno-trapping-math` is off (`FUTARI_GAME_NO_TRAPPING_MATH`): it lets GCC run a float operation earlier than the
  code does, across a change of the host's flush mode the compiler does not know about. `-fno-math-errno` stays.

**Generator options** (`app/perf_overrides.toml`): `fp_moves_modeless`, `fp_compares_modeless`, `fp_mode_lookahead`
(not past a branch), `fcti_inline`, `fp_copy_pairs`, `div_inline`.

**Post-codegen steps** (`tools/post_codegen.py`, sa2-nx's order; each a rename into macros of the generated pch with
a compile-time switch back, `FUTARI_SWITCH_*` / `FUTARI_GEN_*`). Counts for default.xex / one game core:

| Step | What | default.xex | a core |
|---|---|---|---|
| `indirect_calls.py` | every `bctrl` tagged with its site (PC census); listed sites as a guarded direct call | 2,490 sites | 180 |
| `loop_barriers.py` | no barrier where a loop has nothing to protect | 3,450 of 4,987 headers keep one | 2,746 of 3,531 |
| `pch_cold_traps.py` | the trap helper out of line and cold | 1,316 sites | 23 |
| `guest_cas.py` | compare-and-swap without the trailing `dmb ish` | 152 | 9 |
| `flush_mode_flow.py` | flush-mode checks the control flow proves (entry modes for default.xex only: a core is entered through its export table) | 2,084 of 3,781 | 33 of 115 |
| `vec_byteswap.py` | 16-byte reversal as REV64 + EXT | 5,268 | 378 |
| `pch_neon_shuffles.py` | constant word shuffles as one permutation | pch | pch |
| `ro_constants.py` | loads from `.rdata` as constants | 2,031 | 102 |
| `float_loads.py` | a single load straight into a float register | 5,083 | 89 |
| `loop_locals.py` | ctx registers of a call-free loop as locals | 8,463 registers in 1,472 loops | 3,501 in 984 |
| `leaf_locals.py` | ctx registers of leaf functions with loops as locals | 1,839 in 167 functions | 1,545 in 180 |

The localized hot copies (`tools/localize_native.py`) are made from the processed text by the same script: they had
been left at their text of 2026-10-07 by the first regeneration of this round, which the hot guard cannot see (a
stale copy is still a correct copy). The sequence is idempotent (a second run leaves every file's hash unchanged).
sa2-nx's `frame_traffic.py` is not run at all: its elisions change what stale data the guest's stack frames hold.

**Import records are never folded (found here, 2026-10-14).** `ro_constants.py` folds any load whose address lies
in a read-only `.rdata`. A XEX's import records are words of the image the loader rewrites, and in these
executables they stand at the START of `.rdata` (default.xex: 82000400 onwards; `.idata` holds something else). The
loader leaves a function's record alone but writes the address of an imported kernel VARIABLE into its record, and
the generator's image dump is taken after that: it holds the addresses the generator's own process got (12 of the
179 records in default.xex's `.rdata`: 8 in the 0x30...... heap, 4 at 0xFF......). Folding one bakes a run-time
address into the build. sa2-nx's and banjo-nx's dumps hold the same values as each other (0xFFCAD000, 0xFFCAE000,
0x30002000), so today they are probably right everywhere, but only as long as every build
allocates the kernel's variables in the same order, and the PC self-check sees a wrong one only where a session
executes that load. The tool now takes `--xex` and leaves any load that touches a record as a load (63 in
default.xex, 15 in each core). sa2-nx's and banjo-nx's trees fold 86 and 77 such loads; told to banjo-nx, peggle2-nx
(79, changed there) and sasrt-nx (not affected: its records lie outside the folded range).

**GCC's early register allocation is off** (`-mearly-ra=none`, `FUTARI_SWITCH_EARLY_RA`; peggle2-nx's finding,
banjo-nx ships it): devkitA64 GCC 16.1's pass dropped table lookups of the NEON vperm helper in Peggle 2's console
build and overwrote a register. Futari 1.0.0 shows no sign of it (0 self-indexed lookups among 11,484), but the NEON
vperm helper of round 1 is the trigger. `tools/check_switch_build.py` fails on a lookup indexed by one of its own
table registers.

**Run-time keys** (rows of the enhancement table): `futari_cpu_guest_cas_no_barrier` (off puts the `dmb` back without
a build), `kernel_critsec_pcr_owner` (critical sections take the owner from the guest's PCR), `thread_signal_skip_idle`
(Switch only: no notify for an event nobody waits on).

**Self-checks of the PC build** (`app/src/native/futari_codegen_checks.cpp`; a game core is a DLL there and reports
through the executable's exports): every folded constant is still loaded and compared, every promoted call compares
the dispatch table's entry, every proven flush-mode check still tests, every critical section compares its owner
with the looked-up thread. `futari_diag_rdata_check = <folder of the image dumps>` compares the whole `.rdata` of
every loaded module with its dump, import records aside, with each 10 s report: folding assumes nothing writes
there, also for the loads no session reaches. `futari_diag_indirect_census = <file>` writes the census of indirect
calls per module and site. And at every start, on both platforms, `futari_forms_selfcheck.cpp` (sa2-nx's, banjo-nx's
idea) runs the helpers that have an AArch64-only form (conversions, divisions, float copies, the vector helpers,
vperm) on 248 edge inputs against plain C++ references: one `[verify] instruction forms self-check` line, and on a
failure `futari_selfcheck_failed.txt` next to the program, because the release writes no log.

**The lockstep capture and what it can show.** Board 1_0's window (game frames 1200 to 4200) is mostly menus and the
ranking screens, whose layers (background, the scrolling panel, the blinking text) run at their own phases: two runs
of ONE build differ there (250,781 pixels at the same label; the user: "that's just natural variation"). Free play
is worse: the bullet patterns are random, two runs of one build share no frame at all. So a difference in such a
stretch says nothing, and agreement only where the baselines agree among themselves.

**The replay test (`tools/compare_replay.py`).** The one deterministic stretch of play is a replay: the user's slot 5
(Ver 1.5, Xbox 360 mode, the reco_abnormal core) played back in lockstep. Its simulation is fixed by the replay's
data, so every run draws the same game frames, but not necessarily all of them: the game's own frame skip follows
the wall clock, and where one run draws frames 1, 3, 5 another draws 2, 4, 6. Runs of the optimised PC build hardly
skip: three runs of the 1.0.0 reference share 99.9 % of their images. (The unoptimised build of the damaged cache,
below, skipped about every other frame and shared about 70 %.) A build whose game logic differs would lose the
replay's sync at its first difference and share nothing from there on. So the tool gives, per
stretch of 300 recorded frames, the share of a test run's images that some reference run shows within two labels,
next to the same share for a control run of the reference build. The reference is a true 1.0.0 executable, built
from `git archive a86b620` in a folder of its own (`D:\mushihemesama\futari-nx-ref`; the third-party sources and the
game files linked in as junctions: remove those with `rmdir`, never with a recursive delete).

**The registers as C++ locals** (`non_volatile_as_local`, `non_argument_as_local`: phase B2, tried on 2026-10-07 and
reverted after a boot crash). Both are on now. What had gone wrong then, read off sa2-nx's audit tools:
- The scan of that day was linear and marked 72 functions of default.xex and 24 of each core `share_registers`.
  `tools/read_before_write.py` (flow-sensitive) finds ONE real reader in default.xex: sub_8208D82C, the tail of a
  function with a dynamic frame (`addi r12,r31,320`, then its `__finally` funclet). The rest are forms that do not
  depend on the register (`xor rX,rX,rX`, `subfe rX,rX,rX`), which is all the cores have.
- A normal function marked `share_registers` is harmful: the calls of `__savegprlr` / `__restgprlr` are elided by a
  global gate, so it clobbers `ctx.r14-r31` and its callers copy the clobbered values back into their locals. The
  three setjmp callers had been marked that way since 2026-10-06 ("harmless before the option"): two of them save
  with the helpers. The marks are gone; after a longjmp (the image decoders' error path) the caller's register
  locals are as sa2-nx leaves them, like the CR / XER / CTR locals since 1.0.0.
- `tools/non_argument_audit.py`: arguments in r12 to the save/restore helpers (kept in ctx by name), to the EH
  funclets (48 + 18 per core, marked by the generator from the EH tables) and to the stack probe sub_8215C0B4; one
  value returned in r11 by a `__finally` funclet (the generated copy-back takes it). `non_argument_as_local_keep`:
  the stack probe, the EH dispatch sub_82085190 (`mr r12,r4; blrl`), the continuation jumps (sub_8207C9F4, and one
  per core at 880711C4 / 880711D4 / 880811C4) and sub_8219D024 (`addi r3,r11,384`, reached through a pointer).
  With both options on: 0 missing hand-overs, 0 values returned without a copy-back, 0 localized registers read
  before written, in all nine modules.
- Nothing in the app reads or writes `ctx.r0/r2/r11/r12/f0` or `ctx.r14-r31`; the two mid-function hooks of the frame
  wait take `r11` by reference (a local now). The localized copies are made from the new text.
- What it changes that no audit sees: where the 360 stored its caller's registers in a new frame, the saves through
  `__savegprlr` are gone and the inline ones store the function's own fresh locals (zeros). A guest function that
  reads a stack word it never wrote sees something else than before (sa2-nx's tint came from removing the inline
  stores as well, which is not done here). The replay test is the check for that.

**Promoted indirect calls** (`app/indirect_promotions/<module>.txt`, `tools/make_promotions.py`). The census of the
PC build (`futari_diag_indirect_census`) over the eight boards (one game core each: the top menu's Ver 1.5, Arrange,
Black Label and Ver 1.01 entries with both panels load reco, palm, kiniro and hirow, normal and abnormal) and the
slot 5 replay: about 185,000 indirect calls a second in play (SA2: 1.2 million), 89.7 % to the site's most frequent
target. One site carries 45 % of default.xex's calls: the audio mixer's 8211F330 site 1, always to 821493E0.
Promoted: 144 sites of default.xex (their guards catch 85.6 % of its calls) and 17 to 23 per core (78 to 87 %).

**Results (PC, 2026-10-14, the final 1.0.1 code at -O2):**
- Replay test against 1.0.0, game frames 4,800 to 13,802 (about 150 s of the slot 5 replay, reco_abnormal):
  8,355 of 8,356, 8,683 of 8,684 and (verify mode) 8,975 of 8,976 frames found in the reference; the 1.0.0 control
  run misses the same one frame. Rounds 1 to 3 alone (an unoptimised build, before the registers as locals and the
  promotions): 1,914 of 1,915 and 2,012 of 2,012 over frames 4,800 to 7,801.
- Verify mode over the replay: 0 differences. Promoted indirect calls 54.8 million (+13.5 million in the core),
  folded constants 34.6 million (+8.9 million), flush-mode proofs 538 million (+12.3 million), critical section
  owners 5.9 million, read-only data 31.8 million bytes compared, every localized copy and native.
- The Black Label replay (slot 6, the kiniro_abnormal core) against 1.0.0, game frames 4,800 to 10,886: 5,888 of
  6,000 frames found in the reference, the 1.0.0 control run 98.2 % (this replay's screens vary a little between
  runs of one build), stretch by stretch within a point of the control: no divergence.
- All eight boards (one game core each) in verify mode with the final code: 0 differences, about 11 MB of read-only
  data compared per run, 0 different.
- `tools\win\run_sdk_tests.ps1`: 0 failed. `tools/switch_compile_check.py`: the changed SDK and app sources and three
  generated files compile with the Switch compiler; `tools/arm_forms_check.py`: all claims hold.

**The Switch ELF** (build X, no profile: `out/switch_ab/X_1_0_1_nopgo`), against 1.0.0's (build W, with its profile):

| In the recompiled game code | 1.0.0 | 1.0.1 |
|---|---|---|
| `msr fpcr` (flush-mode switches) | 5,084 in 1,177 functions | 2,362 in 1,022 |
| frame pointer set-ups | 32,959 | 11 |
| `fcvtas` (halves away from zero) | 26 in 14 functions | 0 |
| table lookups (all of the ELF) | 11,484 | 5,705 (1,508 of them the two-register vperm) |
| compare-and-swap | `ldxr`/`stlxr` + `dmb ish` | `ldaxr`/`stlxr`; the `dmb` only on the cold branch of `futari_cpu_guest_cas_no_barrier` |
| NRO | 73.2 MB | 67.6 MB |

`tools/check_switch_build.py`: all checks passed (start-up code at 0, Mesa 26.2.2, no vector FMA, 0 table lookups
indexed by their own table register, 0 `fcvtas`). `tools/find_self_loops.py`: 0 self-branches in game code. The static
`dmb` count is unchanged (429) because every compare-and-swap keeps its barrier behind that run-time key.

### Input lag (2026-10-16, the user's question: "Can we improve input lag in the game? such as turning off vsync")

**What the frame looks like** (the latency probe, `rex/frame_latency.h`: per frame the game's first controller read,
its Swap, the present, and on the Switch the display's next vsync; a "[latency]" line every 10 s, and with
`futari_diag_latency_trace_s` the order of every event of 8 frames). The slot 5 replay in real time on the PC, with
the console's settings:

    t =  0.00  guest vblank (the 60 Hz timer) -> the vblank callback bumps the game's frame counter
         0.02  the game wakes and Swaps; the image is presented 0.3 ms later
         0.05  the game reads the controllers (9 reads in 0.6 ms)
         1.9   its frame is done (logic and draws); it sleeps on the frame counter
        16.67  the next vblank: Swap of that frame

The console has the same shape: its 10 s "frame wait" line says the game thread sleeps there 13 of every 16.7 ms.
So the image swapped at a vblank shows controllers read a whole frame (16.4 ms) earlier, and the image then waits
for the compositor's next refresh, at a phase that slides because the 60 Hz timer is not the display's clock.

What is NOT a source: the controller driver samples the HID state when the game asks (padUpdate in GetDeviceState);
the present does not wait on the CPU (the driver hands the compositor the GPU fence, and the compositor scans out our
image directly); presentation already runs at swap interval 0 ("vsync off" in the producer's sense: the Switch has no
tearing mode, the compositor always shows images at its 60 Hz refresh).

**What changed** (each its own key, all on in the test build; `app/futari_switch.toml` lists them commented out):
- `futari_late_input` (adaptive): the frame's first controller read after a Swap waits by what the frame can spare:
  the time to the next vblank, less the longest frame of the last 64 (from that read to the game's wait), less
  `futari_late_input_margin_us` (3 ms). A frame that needs more misses its vblank and shrinks the next delays.
  (A first try delayed VdSwap's return instead: the game kicks the Swap packet after VdSwap, so the present moved by
  the same amount and nothing was gained. The read is the right place: the Swap is already presented then.)
  PC, slot 5 replay in real time: input -> present 16.7 ms -> 5.6 to 8.2 ms, the reads 9.4 to 9.9 ms later, 600
  frames every 10 s as before.
- `futari_vblank_display_sync` (Switch): the guest's vblank comes from the display's own vsync event
  (`viGetDisplayVsyncEvent`) instead of the timer: no sliding phase, no frame shown twice or never when the two clocks
  drift apart. Falls back to the timer if the event cannot be had; a guard drops vblanks closer than 8 ms.
- `futari_vblank_phase_auto` (Switch): the guest vblank as late after the refresh as the image still makes the next
  one. At each refresh the vblank thread checks the last output's fence (`TargetsNative::LastOutputDone`): 3 s of
  images ready in time move the vblank 0.25 ms later (up to `futari_vblank_phase_max_us`, 9 ms); one that was not
  moves it 2 ms earlier and caps it there for 5 minutes. A "[latency]" line gives the phase and the misses.
- Sound: a sound reaches the speaker after the frames queued for audout (`audio_switch_frames_in_queue`, 10 = 53 ms)
  and the three audout buffers (`audio_switch_frames_per_buffer`, new, 4 frames = 21.3 ms each): 80 to 115 ms. The
  defaults stay (the crackle fixes were tuned with them); 2 and 4 would be about 53 ms.

**Not done, and why:**
- Double buffering: the NVK Switch WSI pins the swapchain at 3 images (`wsi_common_switch.c`: min = max = 3), and
  libnx allows interval 0 only with 3 or more buffers, so it needs a Mesa patch (2 images, interval 1) and a driver
  built apart from the shared one. With the game's vblank locked to the display, one image is made per refresh and
  nothing queues behind it: a third buffer then adds no delay, and FIFO with two would only add waiting.
- Sending the frame's GPU work before the Swap (the ring submits once per frame, at the Swap: the GPU starts the frame
  then). An idle flush while the game sleeps could take most of the GPU time out of the Swap -> display path (a few
  ms on the console), but every extra submission halves the frames of command slots in flight and meets deferred
  recording: a renderer change with its own pixel and speed checks, after the console's numbers.

### Banjo's codegen round (2026-10-19, the user: "Banjo has 25+ new codegen improvements ... port them all at once and then verify them")

banjo-nx's `docs/codegen-enhancements.md` lists its generator options and 33 post-codegen steps (sasrt-nx's round of
2026-10-14, banjo-nx's own of 2026-10-15 to 2026-10-19, its CPU codegen agent's "aggressive round"). Everything that
applies to Futari is ported; four steps do not apply (below). All of it is build-time and exact by construction or
checked by the PC build while it runs; every step has a CMake switch back (`FUTARI_GEN_*` in `app/CMakeLists.txt`).

**The generator** (`sdk/src/codegen`, Banjo's hunks on top of sa2-nx 1.0.1's files; Futari's `function_scanner.cpp`
and `project_recompiler.cpp` kept, Banjo's lack sa2-nx's jump-table bound and Futari's DLL handling). New options in
`app/perf_overrides.toml`:
- `rotate_word_shifts`: `rlwinm` / `rlwimi` with an immediate shift and a mask that does not wrap as the plain 32-bit
  shift, extract or rotate (`lsl` / `lsr` / `ubfiz` / `ubfx`: one A57 instruction where the 64-bit rotate of the
  duplicated word took two or three). `sdk/tests/rotate_word_test.cpp`: every shift and mask against the old form.
- `fp_copy_forwarding`: with `fp_copy_pairs`, a single copied through a register (`lfs` ... `stfs`) stores the loaded
  bits in either mode, also with other instructions between and through an `fneg` / `fabs` / `fnabs` of the
  register in place (the sign bit flipped, cleared, set). default.xex: 1,815 copy-form stores.
- `dcbt_prefetch` on, with banjo-nx's operand fix: `dcbt` decodes as {CT, RA, RB}, and the builder read RA as the base
  and RB as nothing, so `dcbt r0,r5` prefetched r0's value. Now RA + RB (r0 as RA is 0). 130 in default.xex, 67 per
  core. `FUTARI_GEN_DCBT_PREFETCH=OFF` compiles them to nothing.
- Always on (corrections, no option): a setjmp site copies every register the function keeps in C++ locals into the
  buffer and back after a longjmp (before, after the image decoders' longjmp the caller's register locals were as the
  failed path left them); a conditional tail branch to a function that keeps r0/r11/r12 in ctx hands them over only
  where the caller localizes them (it already did nothing otherwise); `HostToGuestFunction` inlined into its hook
  without the compiler barrier that made the shim's values go back to memory.
- Available, not used: `non_volatile_as_local_keep` / `_exclude` (registers or address ranges kept in ctx: banjo-nx's
  VMX128 constants and WMV decoder; Futari's audits found nothing that needs them).
- Census: every guest FPSCR write counts its rounding mode (`rex/sys_counters.h`, `kCensusRound*`; PC only), for the
  `[census] FPSCR rounding mode` line of `futari_codegen_checks.cpp` (see `fp_sign_ops.py`).

**Post-codegen steps** (`tools/post_codegen.py`, banjo-nx's order, per module; the steps that analyse the whole
program run on each module separately). Counts for default.xex and one game core (`tools/codegen_round_census.py`
prints all nine):

| Step | What | default.xex | a core (reco_abnormal) |
|---|---|---|---|
| `loop_barriers.py` (narrow) | the barrier clobbers the guest window only, not all memory: ctx and the host stack keep their registers across it (marathon-nx's `PPC_LOOP_BARRIER`) | 3,473 barriers | 2,727 |
| `frame_traffic.py` | marks prologue / epilogue statements only (its three elisions stay off, sa2-nx found them not exact); steps 13-14 read the marks | 1,821 mflr, 2,097 mtlr | |
| `loop_locals.py`, `leaf_locals.py` | vector registers by value (`FUTARI_GEN_VECTOR_LOCALS_BY_VALUE`), the per-module opt-out list `app/locals_optout/<module>.txt` | 1,193 loops, 264 leaves | |
| `vperm_one_source.py` | a vperm of one vector as a one-register lookup | 56 | 0 |
| `fp_sign_ops.py` | fneg / fabs / fnabs as float operations | 171 | 5 |
| `leaf_dead_stores.py` | a leaf's write-back of a register no caller can read is skipped (poison build `FUTARI_GEN_LEAF_DEAD_POISON`) | 491 in 70 functions | 617 |
| `single_floats.py` | single-precision arithmetic in float where the inputs are proven singles | 3,300 of 4,189 | 37 |
| `inline_memcalls.py` | memcpy / memset of a constant size up to 64 bytes inline | 206 of 830 calls | 125 |
| `vec_unaligned_stores.py` | stvlx / stvrx as a few stores instead of byte loops | 464 | 310 |
| `ro_constants_cfg.py` | the read-only constants `ro_constants.py` cannot see across a label (loop heads); each fold still loads and compares on the PC | 602 | 21 |
| `dead_ctx_defs.py` | guest register writes nothing reads, left out (`--abi-indirect`; poison build `FUTARI_GEN_DEAD_DEFS_POISON`) | 895 in 535 functions | 810 |
| `flush_hoist.py` | a loop header's flush-mode check in front of the loop (the PC build tests it like every flush proof) | 65 loops | 0 |
| `lr_preserve.py --rewrite` | epilogue write-backs proven to store the value ctx.lr holds compile to nothing (the PC build compares every one) | 1,941 | 988 |
| `r1_preserve.py`, `r1_local.py` | guest r1 as a local mirror where callees are proven to return it unchanged (the PC build compares after every assumed call) | 3,874 functions | 3,509 |
| `vsel_neon.py` | VMX vsel as one NEON bit select | 13 | 3 |
| `inline_leaves.py` | static inline copies of tiny leaves (12 guest instructions, no calls) in the files that call them | 711 calls | 107 |

**What Futari needed that banjo-nx's tools did not have:**
- *Nine programs.* The whole-program analysis of `leaf_dead_stores.py` (also used by `single_floats.py`,
  `dead_ctx_defs.py`, `ro_constants_cfg.py`, `lr_preserve.py`, `r1_preserve.py`) decides "reached only by direct
  calls" from the image's data words, `lis` pairs, the port's sources and one hard-coded entry point. A game core is a
  DLL that default.xex reaches only through its export (`XexGetProcedureAddress`: the XEX's export table or the PE
  export directory, both relative to the base, so the data-word scan cannot see them). `tools/futari_modules.py`
  gives each module its entry point (`xstart`) and a core its export (one per core, e.g. `sub_88251D50`); the source
  scan takes every `8xxxxxxx` (0x88 is the cores' base) in app/src, the SDK, the tomls and the promotion lists.
- *The localized copies.* Their guard compares the WHOLE context with the original's, and the copies write back every
  register they hold. A step that skips a context write in the original (`leaf_dead_stores`, `dead_ctx_defs`,
  `inline_memcalls`, whose inline path leaves r4-r12 as they were) would make every comparison differ in a register
  nothing reads. Those three leave the 14 functions of `hot_functions.txt` alone (`--exclude`); the value-identical
  rewrites stay in. The copies are made after step 11, as in banjo-nx.
- *memcpy and memset per module* (`tools/find_memcalls.py`): each module links its own LIBCMT; the two routines are
  banjo-nx's instruction for instruction (memcpy 290 instructions, memset 40) in all nine. Their callers already go
  through Futari's natives, which leave r4-r12 and the r1 - 8 slot untouched since 1.0.0: the inline path assumes
  nothing new. `tools/memcalls_liveout.py` lists the guest instructions that read r0 / r4-r12 after such a call
  anyway; the ones looked at are argument spills and `mr r8,r8` hints, so it is a pointer, not a proof.
- *Hooks named in comments* (banjo-nx's fix, in `perf_codegen_post.py`): `#` comments of the tomls and CMakeLists.txt
  no longer make a function "hooked": 132 addresses (the three setjmp callers among them) get direct calls.
- `tools/post_codegen.py` runs the per-module steps three modules at a time (`FUTARI_POST_CODEGEN_JOBS`): about an hour
  on the shared machine for all nine.

**Not taken:**
- `limiter_lazy_log10.py` (the XDK mastering limiter's log10 per sample): Futari has no XAudio (no XAudio string in
  any module's image).
- `switch_goto.py` (threaded dispatch for bytecode interpreters): none in Futari. The console profile's hottest guest
  functions (the audio mixer 821493E0, 82153CB8, 8211F330, a core's 8813A120) have no jump-table switch; the one with
  a 16-way switch (8808D320) dispatches once per call.
- `hot_layout.py`: `FUTARI_SWITCH_HOT_ORDER` already puts the profile's 1,500 hottest functions (host code included)
  first in `.text` with a section-ordering file, which also matches `.text.hot.*`.
- `store_cards.py`: marks pages for banjo-nx's Renut renderer only.
- The r1 proof leaves 835 functions of default.xex open (11 per core): an abort path calls `KeBugCheck`, which returns
  in the SDK (it logs), so the guest comes back with r1 moved by the abort frame (-2,816) and the callers reload r1
  after such calls, as the original code does. Marking `KeBugCheck` no-return would be exact on every path that
  matters but not on that one; left as it is.

**The choices** (`tools/codegen_choice.py`, banjo-nx's rule: measure every function both ways with the build's own
compiler line, never guess): the loop / leaf locals opt-out list, and the functions where the lr write-backs and the
r1 mirror compile cheaper (`app/codegen_choice/<module>_lr_write_same.json`, `<module>_r1_local.json`; without a list
the step applies everywhere it can prove). Each generated file compiled four ways with the Switch build's compile line
of its module, without LTO (default.xex's objects are LTO bytecode); per function locals_choice's loop-weighted cost.
Measured for all nine modules (the user, 2026-10-19: "do the census for all cores, we shouldn't skip"):

| Static loop-weighted cost | loop / leaf locals: on / off / chosen (opted out) | lr write-backs: on / off / chosen (functions) | r1 mirror: on / off / chosen (functions) |
|---|---|---|---|
| default.xex | 2,218,546 / 2,352,566 / 2,118,075 (24) | 293,275 / 303,034 / 291,883 (1,388) | 2,985,214 / 2,982,463 / 2,955,135 (1,185) |
| palm_normal | 1,379,075 / 1,492,092 / 1,378,557 (4) | 131,585 / 136,718 / 130,867 (707) | 2,892,069 / 2,884,321 / 2,870,256 (850) |
| palm_abnormal | 1,349,286 / 1,458,082 / 1,347,348 (13) | 130,901 / 135,748 / 130,031 (695) | 2,878,536 / 2,871,593 / 2,858,152 (836) |
| reco_normal | 1,340,601 / 1,450,006 / 1,338,455 (14) | 130,253 / 135,176 / 129,426 (694) | 2,844,762 / 2,838,044 / 2,823,181 (860) |
| reco_abnormal | 1,342,249 / 1,451,044 / 1,340,103 (14) | 130,239 / 135,082 / 129,359 (693) | 2,846,633 / 2,839,306 / 2,824,834 (852) |
| hirow_normal | 1,320,044 / 1,428,882 / 1,318,148 (13) | 132,250 / 137,509 / 131,542 (699) | 2,823,587 / 2,815,081 / 2,802,647 (853) |
| hirow_abnormal | 1,322,821 / 1,431,181 / 1,320,925 (13) | 132,298 / 137,556 / 131,590 (699) | 2,830,445 / 2,822,263 / 2,809,483 (857) |
| kiniro_normal | 1,335,015 / 1,445,851 / 1,333,687 (9) | 130,232 / 135,274 / 129,477 (707) | 2,848,705 / 2,840,825 / 2,827,859 (838) |
| kiniro_abnormal | 1,334,192 / 1,444,571 / 1,332,833 (9) | 130,267 / 135,271 / 129,486 (707) | 2,855,107 / 2,847,609 / 2,834,250 (836) |

So the locals and the lr write-backs pay everywhere and the choice adds little, while the r1 mirror everywhere is a
slight loss in every module (+0.09 % in default.xex, +0.24 to +0.30 % in the cores; banjo-nx's +0.33 %) and pays only
chosen (-0.7 to -1.0 % against everywhere). Every module has its three lists in `app/codegen_choice/` and
`app/locals_optout/`. The final codegen with all of them (`tools\codegen.ps1 -Force -Perf`, 2026-10-19 23:44) and the
one replay (the user's slot 5, lockstep, 2026-10-20 03:40): 8,618 of 8,618 frames as the 1.0.0 reference.

**A fault the round's first verify run found:** three of the localized audio mixer copies reported a memory
DIFFERENCE. Not a codegen fault: the copies' record mode (`futari_native_hot_record.h`) logs every block that goes
through `REX_RAW_ADDR`, and the guard compares each logged block after the copy's run with the same block after the
original's. `dcbt_prefetch` (off until now) writes `REX_PREFETCH` through `REX_RAW_ADDR`, so every prefetched block
counted as accessed, and the audio buffers another thread fills changed between the two runs. Record mode now
prefetches the plain address; the eight boards in verify mode afterwards: 0 differences in every copy.

**The build's own codegen step ran on every build** (found here; banjo-nx's `keep_unchanged_times` had the same
fix): `keep_times` gave unchanged generated files their times of 10-14, older than today's generator and tomls, and
the generator rewrites `futari_manifest.toml` (an input of its own step) with the same text on every run. The kept
times are now never older than the step's newest input (each module's `codegen.d`, the generator binaries), and the
manifest gets the time of the oldest output.

**Results (PC, 2026-10-19, all steps everywhere they apply: no choice lists yet):**
- `tools\win\run_sdk_tests.ps1` with banjo-nx's tests (rotate shifts, perm1, sign ops, memcalls, single floats,
  unaligned stores): 14 tests, 0 failed.
- Determinism: the build's own fresh codegen (its `rexgluerd.exe`) plus the steps gave every one of the 608 generated
  files the same text as the script's run (sha1 compared).
- Replay test against 1.0.0 (slot 5, reco_abnormal): 7,135 of 7,136 frames (the one frame the 1.0.0 control run
  misses too), 8,907 of 8,907, 7,978 of 7,979, 8,556 of 8,556. In verify mode 8,827 of 8,827: r1 mirror 162.6 million
  checks (+201.4 million in the core), link-register write-backs 38.4 million, folded constants 32.0 million (the
  cfg folds included), flush-mode proofs 574 million, read-only data 31.8 million bytes, 0 differences apart from the
  record-mode fault above.
- All eight boards (one game core each) in verify mode after the fix: 0 differences (r1 mirror 31-106 million checks
  per core, link-register write-backs 3.2-14.9 million, about 11 MB of read-only data each).
- The Black Label replay (slot 6, kiniro_abnormal): 5,888 of 6,000 against one 1.0.0 run with the other at 98.2 %,
  5,997 of 6,000 against both: 1.0.1's figures exactly.
- `[census] FPSCR rounding mode`: the guest writes round-to-nearest only (6 writes in the replay), as
  `fp_sign_ops.py` needs.
- Not run: the poison builds (`FUTARI_GEN_LEAF_DEAD_POISON`, `FUTARI_GEN_DEAD_DEFS_POISON`): the user asked for one
  replay per verification from here on. They are one reconfigure away (`-DFUTARI_GEN_..._POISON=ON`).

### The deko3d renderer (2026-10-19 night, the user: "a fully functional deko 3d native renderer with no ring emulation or edram emulation base yourself on banjo")

Renut's Direct3D-level renderer (github.com/masterspike52/reNut, by reNut and masterspike52; banjo-nx's copy and
adaptation in `app/src/renut_engine/gpu`, reuse permitted by its author) as Futari's third renderer, chosen at boot:
`futari_renderer = "renut"` (`"default"`: `futari_renderer_native` chooses the native PM4-ring renderer or the
emulated Xenos as before). Futari's Direct3D calls run natively through plume (Vulkan on the PC, deko3d on the
Switch): no PM4 ring, no EDRAM model, no register file rebuilt from packets. Built in by default
(`-DFUTARI_RENUT_RENDERER=OFF` leaves it out; `app/cmake/futari_renut_gpu.cmake`). Route and parts: banjo-nx's
`docs/deko3d-native-renderer-kit.md`.

- **The device layout** (`tools/renut_futari_layout.py`): Futari's XDK (2.0.9328) has N&B's public `D3DDevice` below
  0x2B00 and small insertions in the private part, so N&B's offsets shift by region (+0xAC / +0xB0 / +0xC8 / +0xCC /
  +0xD0 / +0xD4; the tool's docstring has the functions that pinned each). The tool's first pass missed the state
  constants of `d3d_native_state.cpp` (blend, colour write, Z, stencil, point, viewport, anisotropy, export state),
  the two minus-one words of the title device, the tiling clear colour, the Swap's front-buffer argument and the
  clear clamp's viewport in `render/draw.cpp`; each translated value was checked against Futari's own Direct3D code
  (the translated offset is used there, N&B's is not) and fixed by hand.
- **The hooks by name** (`tools/renut_futari_hooks.py`): Renut's hooks name N&B's addresses; its tool rewrites them
  as `FUTARI_RENUT_HOOK(Name, fn)` and writes each name's site from `app/src/renut_engine/futari_d3d_functions.txt`
  (70 of Renut's 354 hook names have a Futari function: `d3d_match` pairs scoring 0.9 or more with a clear second,
  plus 25 checked by hand; the render and sampler state setters and getters are left to the game, whose own setters
  keep the device as Renut reads it). Seven addresses are Futari's own hooks (BeginVertices, AllocateInlineData,
  SetSurfaceClip, Clear, Resolve, KickOff, CBlocker_Check): they hand over first thing (`FUTARI_RENUT_YIELD`).
  BeginIndexedVertices (no Renut hook; Futari draws through BeginVertices / DrawVerticesUP) runs Direct3D's own.
- **The eight game cores**: each links its own Direct3D (on default.xex's device). The tool finds each core's copy of
  each mapped function (equal instructions with branch targets ignored, else with immediates ignored; unique in the
  core; its calls land on the core's copies of what default.xex's call): 45 of the 70 in every core, 38 hooked by
  `futari_renut_core_hooks.cpp` (compiled into each core), the other 7 by Futari's own core hooks
  (`FUTARI_RENUT_CORE_YIELD`). A core forwards to the executable's entry table (`FutariRenutEntries`, GetProcAddress
  on the PC, the final link on the Switch). A core's XMemFree frees its headers from the core's own heap, so the
  cores' `D3DResource_Destroy` / `_Release` free through the core's copy (`FutariRenutCallWithFree`).
- **The vertical blank** (`futari_renut_vblank.cpp`): Futari's game logic counts vblanks (its callback
  `sub_8216AD38`, set at device+16752, advances the frame counter at 0x82200560), through Direct3D's vblank handler
  `sub_820A4C08`, which Direct3D's interrupt dispatcher calls after asking a GPU register. Renut has no GPU and the SDK
  no vsync thread without its graphics system: a guest thread calls the handler itself at the display's rate (the
  Switch's vi vsync with `futari_vblank_display_sync`), or once per Swap with `vblank_per_swap`.
- **The SDK** counts the game's Swaps in one place (`rex::latency::SwapCount`, fed by VdSwap and by Renut's Swap):
  `GuestSwapCount`, the lockstep vblank, the recorder and the late input follow either renderer.
  `RexSwitchUnpinCurrentThread` came over from banjo-nx's SDK (the render thread's pin A/B).
- **What is not Futari's**: N&B's widescreen hooks and photo capture are gone; `render/graphics_options.cpp` keeps
  only the anisotropy floor (N&B's per-target scales and formats recognised its shadow maps and SSAO by size).
- **Shaders**: Renut's XenosRecomp with banjo-nx's passes (early alpha), from `tools/renut_dump_shaders.py`'s dump
  (out/renut_shader_dump: 42 containers, the cores' four the same in all eight); on the Switch, banjo-nx's deko3d
  chain (`tools/renut_deko3d_shaders.sh`: SPIR-V dump, `deko3d_shaders.py`, `deko3d_glsl_opt.py` with its 21 passes
  and the PC uam check, `deko3d_shader_package.py`, `renut_switch_shader_cache.py`): all 42 compile, no uam failure.
  Early alpha (`tools/renut_early_alpha.py`) first took none of Futari's 24 pixel shaders ("unsupported: variable
  g_PSC_F1_ColorRGB"): Futari's containers name their constants and samplers, which the translator keeps
  (`#define g_PSC_F1_ColorRGB g_C(2)`, `smpColorMap_Texture2DDescriptorIndex` for banjo-nx's `s0_...`). With those
  read as what they are: 13 copies (each self-checked against the original on random inputs), 11 "little".

- **First light on the PC** (2026-10-20 01:03, plume Vulkan, `futari_renderer = "renut"`, `renut_gpu_backend =
  "vulkan"`): the title screen, the menus and gameplay (inside the game-core DLLs) as the emulated GPU draws them,
  60 game frames a second, about 37 draws a frame, the render thread 0.3 ms and the GPU 0.4-0.7 ms a frame. What it
  took after the build: the game's `GetCurrentThreadId` is Futari's `sub_8208DA10` (banjo-nx's SDK names N&B's
  `rex_GetCurrentThreadId`); Futari asks Direct3D for the AUTOMATIC FRONT BUFFER and passes `device->AutoFrontBuffer`
  (+0x3ABC) to every Swap, which Renut did not make ("an automatic front buffer is not supported": presents without
  an image, no captures): made as Futari's Direct3D makes it (`CreateTexture(width, height, 1, 1, 0, front format, 0,
  D3DRTYPE_TEXTURE)`); on the PC the vblank thread runs above the game's threads and sleeps to 1.5 ms before each
  vblank, then yields (Windows' whole-millisecond sleeps on a busy machine delivered the vblanks in bursts: 35-40 fps).
  Comparisons: `scratchpad renut_lock_video.ps1` (lockstep, the board's pad in game frames, either renderer).
- **The display gamma** (`banjo_renut_game_gamma`, on in Futari): Renut presented without the device's corrected
  gamma ramp (+0x3B6C: Direct3D's display-gamma table over the game's or the default ramp), which the emulated GPU's
  presenter applies from the display registers. Lockstep title frames, mean |difference| per channel against the
  emulated GPU: 12.1 / 10.2 / 9.1 without it (Renut brighter: 112 / 160 / 172 against 99 / 150 / 164); 3.2 / 5.0 / 4.9
  with the ramp fitted to one exponent (x^1.217, a little too dark); 0.8 / 0.8 / 1.0 with banjo-nx's exact form (taken
  from banjo-nx at 03:00: the ramp as a 256 x 1 texture the present looks each channel up in; Futari's offset). What
  is left is the video's compression.
- **The DKSH cache** for the console (`tools/deko3d_variant_cache.py --list out/pc/pipeline_list.bin --glsl
  out/deko3d_shaders/glsl_opt --out out/renut_switch/deko3d_shaders.bin`): Renut's pipeline list of the PC runs (96
  pipelines: title, menus, a stage of board 0_0) gives 33 stage variants, compiled by the PC uam (the console's
  patches), 35 KB next to the NRO; a variant missing there is compiled on the console and appended.

- **The Switch build** (2026-10-20 02:02, `build_switch.ps1 -Pgo none`, renderer on): every check passes
  (`check_switch_build.py`: `_start` at 0, the SDK's Mesa, no vector FMA / self-indexed table / fcvtas in the
  recompiled code; `find_self_loops.py`: 0 in game code; `plume_vma_names.py --check`: no VMA symbol twice).
- **Ryujinx** (`scratchpad emu_run.ps1`, its own data folder `ryu-data\futari`, cache id 0500000046555452): the
  emulator starts an NRO without argv, so Futari did not know its folder (it wrote a default futari.toml at the SD's
  root and found no game files): banjo-nx's SDK fixes taken as they are: `REX_SWITCH_FALLBACK_EXE`
  (`/switch/futari-nx/futari.nro`, `filesystem_posix.cpp`), the guest memory's process pseudo-handle and eager views
  without a loader (`guest_memory_switch.cpp`) and the loader-less heap (`runtime_switch.cpp`); a console with
  hbloader takes none of these paths. Futari's own search also tries `/switch/futari-nx/game_root` (ROOTED: libstdc++
  takes "sdmc:/..." as a relative path, and `exists()` on it throws). And any C++ exception kills Ryujinx: libgcc's
  unwinder has `mrs x1, gcspr_el0` behind a `chkfeat` branch that hardware takes, but the emulator translates the
  block and stops ("Unknown MRS 0xD53B2521"): an invalid futari.toml (toml++ throws) or an `sdmc:/` path is enough.
  deko3d itself came up at the first try: "Renderer: deko3d on deko3d (Tegra X1, Maxwell GM20B)", the DKSH cache's
  33 variants loaded.

- **Coverage on the PC** (lockstep videos, `scratchpad renut_lock_video.ps1`): the title, the menus, and play in
  reco, palm_abnormal, kiniro_normal, hirow_abnormal and Black Label (kiniro_abnormal, Maniac): no shader missing from
  the cache, no draw dropped, no renderer warning. Every run used the same 96 pipelines.
- **The staged build**: `out/switch_ab/AA_renut_deko3d` (NRO, DKSH cache, ELF and map; not published).
- **Since then** (04:57 build): banjo-nx's stale texture-header fix (`banjo_renut_device_swizzle`: a header the game
  built on the stack describes a later pass by render time; where its dwords 0, 3 or 5 differ from the device's fetch
  constant, the device's is sampled; Futari never hit it on the PC); and on the console the vblank's phase after the
  display's refresh found by measurement again (`futari_vblank_phase_auto`, the native renderer's algorithm, with
  "finished at its refresh" from Renut's present fences, `Video::PresentDone`).
- **Banjo's shader and render-thread optimisations in Futari** (2026-10-20 morning, the user: "use the shader
  optimisations from banjo + render thread optimisations"). Shaders: Futari's chain runs all 27 of banjo-nx's text
  passes (`tools/deko3d_glsl_opt.py` as banjo-nx has it at 02:10, `guardsel`, `deadcomp`, `bonesel`, `txq`, `earlyz`
  and the rest), then uam with banjo-nx's flags (447: PRMT, GVN, uniform branches). `tools/deko3d_shader_stats.py`
  over Futari's pipeline list, against the same GLSL without the passes: the small pixel shaders 41-49 -> 11-19
  instructions, the heaviest (D28D) 307 -> 216, the vertex shaders median 154 -> 147. What does not fire, and why:
  `txq` (no Futari shader asks a texture's size), the bool specialisations (no Futari shader has a bool constant),
  `aw4` / `bonesel` and banjo-nx's newest uam flag 1024 (`banjo_deko3d_uam_wide_rows`, bone rows through the address
  window: Futari's vertex shaders read their streams as vertex attributes, 0 draws through the window). Render
  thread: every Renut key banjo-nx's shipping `renut.toml` sets has the same value in Futari, except
  `banjo_late_input` (Futari's own `futari_late_input`), `banjo_renut_game_gamma` (on: the emulated GPU's look) and two
  profiling diagnostics; the keys Futari lacks are banjo-nx's ring emulation and its game's CPU natives. In the
  emulator's report: pushed constants on (every draw), the ring cleaner on its thread (956 ranges a 10 s, 0 on the
  render thread), DKSH cache hits and 0 uam compiles, render thread 1.3-2.2 ms a frame. Not taken, with numbers: the
  store cards (`banjo_renut_store_cards`, banjo-nx's proposal 1). They need banjo-nx's `tools/store_cards.py` in
  Futari's codegen, a card store after every guest store outside the stack (the game thread, Futari's limit), to
  save the render thread's texture hashes. Futari's (a counter for it, the "[renut] textures without store cards"
  line): about 50 hashes a frame in gameplay, 1 KB each (tiled textures' sampled hash), and no dynamic re-uploads
  (banjo-nx hashes about 150 MB a second, its 1920x1920 linear font cache). Futari has no vertex or index copies to
  prove either (0 cached streams: its draws are Direct3D's BeginVertices, data the game writes fresh each frame and
  Renut converts into the upload ring at the call). So about 0.1-0.15 ms of the render thread for a cost on every
  frame of the game thread.
- **Renut's keys under Futari's masters** (2026-10-20): its optimisations read their own cvars, and only a few went
  through `EnhancementActive`, so `futari_cpu_enhancements` / `futari_noncpu_enhancements` = false left most on (the
  same holds in banjo-nx for its masters). 52 of them are now rows of `src/futari_enhancements.cpp`'s table (CPU: the
  render thread's work, its cores, NVK's; non-CPU: the GPU's, memory, input latency), so the masters and the
  generated configs cover them; each row's ON value is its code default, so nothing changes with the masters on.
  Not rows: Renut's fixes, its settings, and banjo-nx's game-specific ones (fur and light shader lists, fur shells,
  its display thread, store cards).
- **banjo-nx's latency trim and GPU-done probe** (2026-10-20 06:30, from its tree as it shared them):
  `banjo_deko3d_latency_trim` (off, as banjo-nx's until a console measures it; not a table row, since the built-in
  defaults would put it on; its check honours the non-CPU master): where the GPU is the limit, after a present the
  render thread waits until the GPU has finished all but the frame's last kick list, so the next frame is recorded
  and the game thread released just in time (a fence per kick submission; backs off for 60 frames if the GPU runs
  dry). Nothing at a frame cap, which is where Futari runs. The probe (`rex::latency::NoteGpuDone`, Futari's SDK):
  the `[latency]` line's "input -> GPU done", the earliest a frame can be shown; on the PC 0.1 ms under "input ->
  present" (no GPU queueing). On the console it says whether Futari ever waits on the GPU. A list ends at a kick
  only with 200 draws in it (`kKickSubmitDraws`, video.cpp): in the menus and the game over (about 70 draws a frame)
  each frame is one list submitted at its present, and neither key acts; in play (250-650 draws a frame) they
  do. Ryujinx with the trim on, play windows: 270-530 lists submitted at kicks and 1-3 of ~500 frames held back a
  10 s (the emulator is not GPU-bound); the vblank thread "priority 0x2C, pinned to core 2"; 56-60 fps, no error.
  (A first reading of the menu windows only said "0 at kicks": wrong for play.)
- **Delta captures** (`futari_renut_delta_captures`, CPU, 2026-10-20 08:40; banjo-nx's render-thread proposal 15,
  which it left for later): every queued draw copied the device's first 12,888 bytes (`kDeviceSnapshotSize`, 0x3258
  in Futari's layout) on the game thread, 3-8 MB a frame at Futari's 250-650 draws, and the render thread read
  them back from another core's writes. Now a capture keeps what Renut's recorded command-buffer ops already keep
  (`guest/command_buffers.cpp`): nothing before the fetch constants (the draw paths read none of it), the fetch
  constants and the tail as the 64-byte chunks that differ from the game thread's shadow of the previous draw
  capture, the float constants as the groups `m_Pending[0]` / `[1]` mark written (read before Renut clears them).
  The render thread applies them to its own copy in the queue's order and draws from that. Whole captures: the
  first, after a capture taken but not queued, after a command-buffer op was recorded (it consumes the masks;
  `RecordedOpCount`), and for clears and resolves (as before). On the A57's one load and one store pipe the compare
  is ~7 KB of loads where the copy was 12.9 KB of loads and stores, and the render thread reads a copy in its own
  cache. PC, board 0_0 in verify mode (`futari_cpu_verify`: each capture also carries the whole snapshot, the
  render thread compares): 0 mismatches in 1,394,547 draws (150 s), 175-203 bytes a draw (1.4-1.6 % of the whole).
  Then every core in verify mode (boards 0_0-3_1: reco, palm, kiniro and hirow, normal and abnormal, and the slot
  5 replay): 16.7 million draws, 0 mismatches, no error.
- **The caches survive HOME** (banjo-nx 06:50-07:00, from Peggle 2's report; taken at 07:15): the DKSH cache's
  appends and the pipeline list are synced to the card (`fsync`; the list at most once a second, after the present),
  where a flush alone left them in the file system service and a game closed with HOME lost them (the same pipelines
  and variants built again at every start). The list's records are also loaded with the keys this configuration
  makes (a record written with another key on would be built for no draw), and the first 64 run-time uam compiles
  are logged with their constants (`tools/deko3d_variant_cache.py` names what a merge compiles). Futari keeps its
  tag bnx3 and flags 447.
- **sa2-nx's Renut CPU work** (2026-10-20 14:00, the user: "do all of sa2's new cpu improvements";
  `D:\sonic adventure 2\sa2-nx\docs\renut-cpu-work-for-banjo.md`): taken through banjo-nx's port of it (its tree had it
  by then, keys `banjo_renut_*`): the device snapshot deltas with the constants by the chain and the narrow compare
  (items 1-3), the NEON line compare (4), the capture pool in batches (5), the hooks' fast paths (6), the render
  thread woken every 16 draws (7), the next item prefetched (8), the done signal only with a waiter (9), one clock
  read a batch (10), the chain's reuse (11), the pushed-constant fast path and the shared constants' inline compares
  and skips (12-13), the smaller ones (14), and the Switch core layout (16, the SDK's `RexSetGuestThreadCoreLayout`,
  `RexSwitchSetThreadMaskOf`, and every guest thread registering itself at start: the drawing thread alone on core
  0, the other guest threads on cores 1-2 preferring 2; the log line says whether the drawing thread is the main
  thread, which the layout assumes). Items 17-18 are store cards (none in Futari), 19 the driver-less build (done).
  Futari's own delta captures (`futari_renut_delta_captures`) gave way to them. Futari's layout: the narrow compare's
  unread private range (10880-11776) holds in Futari too (its fields there sit where N&B's do; its later private
  fields are shifted further up, the vertex declaration at 11992), and the snapshot's 0x3258 bytes make 42 read
  chunks (N&B 39). **Futari's addition, `futari_renut_deferred_chain`**: sa2-nx's chain left Begin*Vertices draws whole
  (queued later than made), which are 88 % of Futari's draws (115,582 of 131,000 a window); they are now deltas of
  the chain too, made under its lock and queued by `FlushDeferredVertices` under it (every hook flushes its device's
  deferred draw before making another). PC, board 0_0 lockstep, `banjo_renut_snapshot_deltas_verify` and
  `banjo_renut_chain_reuse_verify` on: every draw a delta (0 Begin*Vertices draws whole), 225-291 bytes a draw (12,888
  whole), every draw verified (127,296-167,473 a window), 0 differ, no reuse difference, 60 fps. banjo-nx, peggle2-nx
  and sa2-nx took it the same day (banjo-nx: ~90k delta draws checked, 0 differ; Peggle: 13,088 -> ~540 bytes a draw).
  After the 14:32 NRO: banjo-nx's fix of a race in the chain's pipeline reuse (the prebuild epoch read before the
  lookup, not after: a specialised pipeline finished in between left the next draw on the generic one; the same
  pixels), in the source for the next build. And (banjo-nx's finding, 15:00) a guard for more than one D3D device: the
  chain is one for all devices, and a deferred delta is made at Begin but queued at its device's next hook, so a draw
  on another device in between would rebase the chain under it (N&B has two devices: banjo-nx saw hangs and turned
  its copy off). Futari has one (one "D3D device (type 1)" in every run, all eight cores), so the 14:32 NRO is safe;
  still: the chain counts its captures (`DeltaChain::made`), a deferred delta records the count, a flush that finds
  the chain moved queues the whole device instead, and after the first device switch Begin*Vertices draws are whole
  copies (`multi_device`, logged once). PC verify after it: 223-245 bytes a draw, 0 differ, no warning.
- **banjo-nx's input-latency set and a 2-image swapchain** (2026-10-20 13:00, relayed by banjo-nx as the user's
  request: "take all of Banjo's input-latency changes, ON by default, and develop a double-buffer option, ON"):
  - taken, on: `banjo_deko3d_eager_kicks` (lists of 100 draws instead of 200, up to 6 a frame, each submitted at its
    own kick; Futari's 250-650 draws a frame make 2-6), `banjo_late_input_gpu` (the read later by the GPU queue's
    slack, `banjo_late_input_gpu_margin_us` 6000; part of banjo_late_input's delay, so it acts with that one, below;
    REMOVED at 17:30, the user's call through banjo-nx: on banjo-nx's console it found false slack where the GPU just
    holds 30 fps, 29.7-30 -> 24.3 fps for 1-11 ms less lag; in Futari it never acted, Futari's own late input being the
    one installed),
    and banjo-nx's other changes in the same files (2D-array views of
    layered images, from SASRT; the run-time variant log); `banjo_deko3d_latency_trim` stays off (it starved the GPU
    with eager kicks on banjo-nx's console).
  - `banjo_late_input` is on but acts only with `futari_late_input` off: on the PC it delayed the read 0.0 ms in
    Futari ("delayed 0.0 ms on average", input -> swap 16.7 ms), its slack being the render thread's wait in the Swap,
    which Futari's game thread, waiting on its own vblank, never makes; Futari's own gave ~6 ms input -> swap in the
    same scenes (07:05 runs). One read delay at a time (both install through `rex::input::SetFrameReadDelayFunction`).
  - banjo-nx's display sync (`banjo_display_sync`) paces a frame limiter Futari does not have (the game paces itself
    on its vblanks); Futari's equivalent, the display-synced vblank with its measured phase (`futari_vblank_display_sync`,
    `futari_vblank_phase_auto`), is on, and works on a console since the default window fix below.
  - `futari_renut_double_buffer` (on, `futari_renut_shim.cpp`, `backend/video.cpp`): the swapchain with 2 images
    instead of 3 (the frames in flight + 1). The acquire is a queue wait on deko3d (`dkQueueAcquireImage`): the GPU's
    next frame waits for the display to give the image before back, one image less queued before the display; a frame
    that misses a refresh costs a whole refresh there. PC: "Swapchain: 2 images", 59-60 fps. The console's numbers
    (the `[latency]` line's "input -> GPU done" and the frame rate in heavy scenes, against `futari_renut_double_buffer
    = false`) decide whether it stays on.
- **No Vulkan driver in the Switch build** (the user, 2026-10-20 11:40: "remove the vulkan driver from the switch
  builds"): `FUTARI_SWITCH_NVK` (app/CMakeLists.txt, default OFF) sets the SDK's `REXGLUE_SWITCH_LINK_NVK` and plume's
  `PLUME_SWITCH_LINK_NVK`, which leave Mesa's `libvulkan.a` out of the link (the SDK's `rex::nvk` and plume's driver
  group). The code that still referenced it, the linker's list: `vkGetInstanceProcAddr` and `vkDestroyInstance` (the
  SDK's Vulkan code), `vk_icdGetInstanceProcAddr` and `vk_icdNegotiateLoaderICDInterfaceVersion` (plume's Vulkan
  back end), and `posix_memalign`, which Mesa's archive had supplied to libavutil. `sdk/src/ui/switch_vulkan_stub.c`
  answers them as a system without a driver (`vkCreateInstance` -> `VK_ERROR_INCOMPATIBLE_DRIVER`, no extensions or
  layers, every other function unknown; `posix_memalign` on newlib's `memalign`). NRO 70.4 -> 56.0 MB (Mesa was 21.6
  MB of code and data). Only Renut on deko3d draws now: the native renderer and Renut on NVK need `-DFUTARI_SWITCH_NVK=ON`.
  `tools/check_switch_build.py` reads the option from the build's cache and then expects no Mesa string. Ryujinx:
  "Renderer: deko3d on deko3d", the vblank on the display's refresh, no error. (peggle2-nx did the same at the same
  time and found the same five symbols.)
- **The display's vsync event on the console** (banjo-nx's console logs, 2026-10-20 11:20): a second
  `viOpenDefaultDisplay` FAILS on the console while libnx's default window (`nwindowGetDefault`, which the deko3d
  swapchain and Mesa's WSI use) holds "Default" open; Ryujinx answered it, which hid it. So on a console both of
  Futari's vblank threads (Renut's and the native renderer's, `futari_vblank_display_sync`) fell back to the 60 Hz
  timer, and `futari_vblank_phase_auto` never ran (1.0.1's NRO too; Futari's only console logs, 2026-10-07, are
  older than the key). banjo-nx's fix taken (`renut_engine/display_sync.cpp`): `__nx_win_init`, `__nx_win_exit` and
  `nwindowGetDefault` defined there (all three, so libnx's default_window.o stays out of the link; libnx's init.o
  calls the two through weak references), libnx's own calls in its order, and the window's `ViDisplay` kept;
  `banjo::display_sync::DefaultWindowDisplay()` (Futari's) gives it to both vblank threads, which open a display of
  their own only without it. In the shared Ryujinx (banjo-nx's Ryujinx.HLE.dll of 11:23) the vi vsync event now
  comes on a 60 Hz grid, as the console's, not after each composition.
- **The vblank thread on the console** (`futari_renut_vblank_core`, 2026-10-20): banjo-nx's display thread's
  placement (priority 0x2C, the render thread's; pinned to the background core 2) for Futari's vblank thread
  (display-synced or on its timer), which was created at the game threads' priority on no core of its own (Horizon
  does not slice between threads of one priority: a vblank could wait behind a busy game thread). -1 = as before.
- **The game thread with Renut against the native renderer** (PC, `scratchpad ab_gamethread.ps1`: board 0_0 in real
  time, renderers alternated): not measurable on this PC. Two runs of Renut differed 2x in the game thread's sleep
  (5.9 against 3.0 s in 10 s), more than the renderers did, and "input -> swap" moves with the adaptive late input.
  The console's profile config (`futari_profile.toml`, now with Renut's `renut_gpu_profile = 10`) is the measurement.
- **The picture after the morning's changes** (PC, 09:20 build, delta captures on): the title's 8 frames (lockstep,
  from frame 600, full size) pixel for pixel those of 04:46 (0.00 / 0.00 / 0.00), 0.82 / 0.84 / 1.01 levels from
  the emulated GPU as before.
- **Both masters off with Renut** (PC, 09:22, board 0_0 lockstep): "0 of 165 active", 57-60 fps in play, the picture
  as with them on (5.6 levels, the runs' spread); one "[error] BaseHeap::Release failed because address is not a
  region start" from the game's loading thread at the stage load (`futari_355.log`). Not again in four more runs
  (each master alone, both twice); never in the native renderer's masters-off runs of 2026-10-08. Intermittent:
  if it comes back, the D3D resource frees through the cores' XMemFree (resource.cpp) with the CPU keys off first.
- **One stall, not reproduced** (PC, 09:15, `futari_349.log`, lockstep title capture with
  `futari_renut_delta_captures = false`): after the first 10 s report no thread logged again (the audio threads'
  lines stop too) and the video never finished. The same command again, and three more runs with the key on: normal.
  Not the pipeline list's new `_commit` (no record was appended at the title). If it comes back: the log writer and
  the shared D: disk first.
- **Renut keeps the game's logic** (PC, 2026-10-20 08:00): the slot 5 replay (reco_abnormal), lockstep, frames
  15,600-16,800 (the replay's last stretch, where a divergence would have grown) with Renut and with the emulated
  GPU: the same play and the same counters (x1704, x1873, x2002, x2499, x2728, x2785, x2887 in both), the frames'
  means equal, the last ones 1.1-1.6 levels apart (the renderers' tone); the frames 10-14 levels apart are a frame
  of the game's own wall-clock frame skip off (2618 against 2616). A desynced replay is 40+ levels off throughout.
  (`compare_replay.py` cannot judge it: its references are image hashes of the emulated GPU.)
- **Lockstep is slower with Renut on heavy frames** (not in play): that replay ran 47-53 fps in lockstep
  (`vblank_per_swap`) with Renut, 60 with the emulated GPU, and 58-60 fps with Renut in real time at up to 650
  draws a frame. Lockstep ties each vblank to a Swap, and with Renut the game thread is released by the render
  thread's present (vsync on), the likely cost (not measured further: it is the test mode only). Runs of Renut in
  lockstep need ~20 % more seconds to reach a frame. (The 37 fps reco_abnormal run of 01:06 predates the
  PC vblank fix.)
- **A second game in one session** (PC, `scratchpad padb_swap.txt`: board 0_0's game to its game over, "no" to the
  replay, the ranking, the title, the menus, a second game): the renderer goes through all of it without a warning.
  The second game was the same core each time (reco_normal); a second core in one session is still untried: after a
  game over the ranking and title screens do not run in lockstep (the same presses met other screens in two runs),
  so the presses to the other shot type miss. The replay menu would do it (slot 5's replay, then slot 6's Black
  Label one), but slot 5's replay is still in play at frame 18,700 (lockstep, past 5 minutes of play): a long run,
  not tried. The short way, found by discovery runs but not run to the end (the user called verification done):
  in play Start opens RESUME / RESTART / OPTION / RETURN TO TITLE (three downs, A, A for yes) -> the Score Attack
  menu; six downs to RETURN TO TOP MENU, A, A for yes -> TOP MENU (Ver1.5 / Arrange / Black Label / Ver1.01: the
  boards' first digit; the second, "right" on the next screen, is normal / abnormal; the shot type picks no core).
  `scratchpad padb_pause.txt` / `padb_swap_top.txt` (board 1_0's presses from its top-menu "down" on).
- **Open**: a console run (only Ryujinx so far, where the display's vsync event is not used, so the measured phase is
  untried); Renut on NVK (`renut_gpu_backend = "vulkan"`) on the Switch is untried; the PC keeps the emulated GPU as
  its default. **For a release**: this NRO embeds Futari's translated shaders (`out/renut_switch`: the SPIR-V cache
  and the deko3d GLSL package) and its DKSH cache is game data too, where 1.0.x's NRO carried none (the installer
  builds the shader library from the player's copy). Before publishing, the installer would have to make these from
  the player's copy (Renut's XenosRecomp, SPIRV-Cross, the GLSL passes and uam) and the NRO read them from the SD
  card; nothing here is published. A way found by peggle2-nx (2026-10-20, its installer): the installer's
  dxc_web.wasm builds Renut's deko3d SPIR-V as it is, with "#define BANJO_DEKO3D 1" and "#define RENUT_RECOMP 1"
  put before the translator's HLSL and, after SPIRV-Cross, the forced y flip of gl_Position undone (Peggle's GLSL
  after deko3d_glsl.py and deko3d_glsl_opt.py was then byte for byte the native chain's); SPIRV-Cross builds to wasm
  with em++; Renut's translator needs a front end without DXC (peggle2-nx's tools/renut_hlsl_frontend.cpp and
  renut_hlsl_pch.h). uam for the DKSH cache would be the remaining piece (or the console compiles the variants once
  and appends them).

### PGO of the final Renut build (2026-10-20 evening, the user: "we are ready for final build so please give me pgo instrumentation nro with all logging off")

- **The instrumented NRO**: `build_switch.ps1 -Pgo generate -BuildDir out\switch_pgo` from this code (AD plus the
  GPU-queue late input's removal), staged as out/switch_ab/AE_renut_pgo_generate with the player's futari.toml and
  the DKSH cache. Checks: check_switch_build.py passed, find_self_loops.py 0 in game code, plume_vma_names.py 0.
- **Logging off**: the built-in settings already had every log and profiler off; `renut_memory_report = 0` (Renut's
  30 s memory report: the log level skipped its line, not its queries) joined that list and the verify configs'
  profiler-off block (tools/make_enhancement_configs.py). The session must run the player's futari.toml: a test
  config turns logging and the sampler on, which the release does not run.
- **Build note**: this shell's DEVKITPRO was MSYS's `/opt/devkitpro`, so the first configure stopped at "libnx linker
  specs not found"; set `$env:DEVKITPRO = 'C:/devkitPro'` before build_switch.ps1 (a failed configure leaves a
  CMakeCache.txt that stops the next one from configuring: remove that folder).
- **Next**: the user's session -> sdmc:/switch/futari-nx/pgo/*.stream; pgo\*.gcda (1.0.0's) moved aside,
  tools\win\pgo_merge.ps1, `build_switch.ps1 -Pgo use` in out\switch. No code change in between (settings in a
  toml, such as futari_renut_double_buffer, can still change: they are not code).

### The release's shaders beside the NRO (2026-10-20 night, the user: "look what peggle did for deko3d shaders on the web installer do the same")

- **NRO**: peggle2-nx's design. `FUTARI_RENUT_RELEASE_SHADERS` (app/cmake/futari_renut_gpu.cmake, on by default) builds
  with tools/renut_release_shaders.py's files (out/renut_switch_release): no GLSL package in the NRO (Futari has no
  shader of its own among the game's, unlike Peggle 2's Bink trio) and the shader table without SPIR-V (deko3d reads
  none). banjo_renut_deko3d.cpp is peggle2-nx's loader: `deko3d_glsl.bin` beside the NRO first, then a built-in
  package; a package without a dictionary (the installer's raw zstd frames) is read too; MissingGameShaders() is
  logged at the renderer's choice (banjo_renut_select.cpp). The ELF has no `.rodata.banjo_deko3d_glsl` /
  `banjo_renut_spirv` section; g_compressedSpirvCache is one zero byte.
- **Installer** (futari-nx-installer lib/deko3d.js, peggle2-nx-installer's): the 42 distinct containers stored in the
  nine images (lib/shaders.js imageContainers) -> wasm/renut_hlsl (Renut's translator, tools/renut_hlsl_frontend.cpp,
  peggle2-nx's: its HLSL is XenosRecomp's BANJO_HLSL_DUMP_ONLY text without the first "// pixel shader hash=" comment,
  checked on all 24 pixel shaders) -> lib/early_alpha.js (tools/renut_early_alpha.py + tools/early_alpha.py in
  JavaScript, Futari's own step: 13 of the 24 pixel shaders get a copy) -> DXC (the page's, with the #define preamble;
  the vertex shaders' y flip undone) -> SPIRV-Cross -> lib/deko3d_glsl.js (deko3d_glsl.py + deko3d_glsl_opt.py:
  Futari's copies are byte for byte peggle2-nx's) -> lib/deko3d_package.js. The translator, the GLSL tools and the
  shader_common.h are the same files in both ports, so peggle2-nx-installer's wasm builds serve as they are.
  Against the native chain (out/deko3d_shaders/glsl_opt): 40 of 42 byte for byte; ps_602DCD1805371872 and
  ps_B890268C9A9044DA differ where the page's DXC (v2025.1) numbers SPIR-V's values (SPIRV-Cross's _53 -> _54 ...),
  and ps_B890 builds one texture coordinate as vec2(t.x, t.y) instead of vec2(t.xy): the same shaders. The release
  is tested with the page's package (Ryujinx 2026-10-20 night: 42 shaders read beside the NRO, 24 uam compiles, 0
  failed, 20 saved to the DKSH cache; then the emulator's known double-buffer slowdown after ~90 s, as with AB/AC).
  The manifest's 1.0.1 entry: "shader_library": false (no 1.0.0 library: the NRO links no Vulkan driver) and
  "deko3d": { cores, shaders, containers_sha256, package_sha256 }.
- **PGO**: Renut's code (futari_renut_gpu) is not an instrumented target (only futari_recomp and futari are), so the
  loader change costs the profile nothing.

### Candidates after 1.0.1 (banjo-nx's relay of 2026-10-14; not in 1.0.1, which is the CPU rounds only)

- **A native deko3d renderer** (the user: Futari "will switch to eventually"; banjo-nx's offer of 2026-10-19). Its
  kit: `D:\Banjo kazooie port\banjo-nx\docs\deko3d-native-renderer-kit.md` (after `docs\rexglue-api-renderer-guide.md`):
  Renut's Direct3D-level renderer (about 125 hooks, `reNut\config\renut_gpu_funcs.toml`, bodies in banjo-nx
  `app\src\renut_engine\gpu\d3d\hooks`) on plume's deko3d back end, Renut's XenosRecomp shader ABI, a DKSH cache built
  on the PC. Futari specifics from banjo-nx: the hook set generated for each of the nine Direct3D copies, all
  dispatching to one renderer; check whether each core creates its own device (Renut's CreateDevice hook initialises
  once) and release a core's resources at unload; the remaining PM4 (kick, fences, Swap, lock flush, the inline-data
  allocator) is what those hooks take over; take Renut's shader translator with the renderer (55 shaders); the
  display-locked vblank and `phase_auto` carry over (banjo-nx's `display_sync.cpp` is the same idea for 2+ refreshes).
  Credit reNut and masterspike52 (the user reports reNut's author permits reuse in the user's ports).

- `banjo_gpu_edram4_split_tiles`: a Clear or draw with one proven rectangle that covers part of a tile another view
  owns takes the tile without a transfer (split ownership, healed only when needed). Banjo: EDRAM tiles moved per
  frame 176 -> 31, exact. Applies where surfaces are not whole tiles wide/high (Banjo: 1024 in pitch 1040). Futari's
  main targets are 1280x720 (16 x 45 tiles): count Futari's edge-tile transfers first.
- `banjo_gpu_proof_depth_only`: in EDRAM mode 5 (depth only) the alpha-test / alpha-to-coverage bits must not reject
  the full-overwrite proof (no pixel shader runs). Banjo's loading screens: 16 ms GPU per Swap on the console.
- banjo-nx's relay of 2026-10-16 (its docs/codegen-enhancements.md steps 23-29, tools in banjo-nx/tools): the XDK
  mastering limiter computing log10 per sample but using it only on its rare pow path (`limiter_lazy_log10.py`,
  about -5 % of Banjo's guest time; look for a log10 + pow function in Futari's audio code first); r1 as a local
  mirror per function (`r1_preserve.py`, `r1_local.py`, -0.41 %); proven redundant epilogue LR write-backs
  (`lr_preserve.py --rewrite`, -0.18 %); flush checks hoisted in front of call-free loops (`flush_hoist.py`);
  threaded dispatch for bytecode interpreters (`switch_goto.py --thread`); and a ring "prover" that fingerprints
  guest memory on another core as soon as the game submits (`banjo_ring_prover.h`).
- Deferred recording (on in Futari since 1.0.0): slightly worse on Banjo's console, where only about 18 % of the
  render thread is driver calls. Futari's console number for the key alone was never taken; an in-run A/B on the
  console would say whether it pays here.

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

## The display queue stuck one image deep (`futari_renut_queue_drain`, 2026-10-28)

Found on Ketsui's console (ketsui-nx, docs/ketsui-notes.md "Frame pacing"), with the same renderer and the same
2-image swapchain (`futari_renut_double_buffer`). After one image misses its refresh (a load, one slow frame), every
later image misses it too while the game keeps up 60 fps. Each image's present pass waits for the swapchain image the
display still shows, and the display gives that one back only when the image before is latched, at that same
refresh. It looks smooth, but every image is shown a refresh late (about 17 ms of input lag), and
`futari_vblank_phase_auto` sees every image as a miss. The sign in a log: the `[latency] guest vblank` line's "images
not finished at their refresh in all" rising by ~600 every 10 s in play. Only a refresh without a new image empties
the queue.

`futari_renut_queue_drain` (on; a non-CPU row, the master turns it off): the vblank thread counts new images in a row
not finished at their refresh. After 60 (a second), it skips one guest vblank (`futari_renut_vblank.cpp`,
`FromDisplay`), so no frame starts in that refresh: the image before still makes it, the queue empties, and every image
after it is shown a refresh sooner. Not again for 5 s; when the queue fills again within twice that gap, the gap
doubles, up to 5 minutes (real lateness, which a drain cannot help). The same line counts the drains ("queue drains
in all"). Ketsui's console run with its form of it (a Swap fence held one vblank, as Ketsui waits for that fence): three
drains in the boot, the title and the first load, then no late image for the rest of the run, and present to scan-out
from ~27 ms to 10-11 ms. The user (2026-10-28): "the input lag fix worked for sa2. so put it in mushi and guwange and
release it without testing".
