# Xbox 360 → Switch ports: optimisations

Shared notebook of the five ReXGlue static-recompilation ports (masseffect-nx lineage: PM4-ring native Vulkan renderer
on NVK, ReXGlue SDK). One section per port, written by that port's session; the general section and the known-issues
list are shared.

**Rules for editing this file** (several sessions write to it): re-read it right before editing, change only your own
section (between its `<!-- BEGIN ... -->` / `<!-- END ... -->` markers) or append rows to the shared tables, never
rewrite another section. Each row: what it is, its key (every optimisation has its own on/off key + a CPU / non-CPU
master), how it was verified, the measured effect, status.

Status words: **console-verified** (measured on a Switch), **PC-verified** (self-check / verify mode 0 differences, PC
runs), **pending console**, **experimental** (off by default), **broken** (known issue, do not use).

---

## Known issues (all ports): optimisations that broke something

<!-- BEGIN KNOWN ISSUES (append-only, any port) -->
| Optimisation | Port(s) | What went wrong | Fix / status |
|---|---|---|---|
| Guest loads/stores without `volatile` (post-codegen pass, masseffect-nx t153) **alone** | Futari (seen), SA2 (40 loops found) | GCC -O3 on the Switch hoists guest polling loads out of loops: 172 functions in Futari became `b .` infinite loops, e.g. D3D's `while (device[11012] != 0) {}`; the console hung at boot (0 swaps, game thread 100 %). clang on the PC did not, so PC tests passed. | REQUIRED companion: `REX_LOOP_BARRIER()` (asm memory clobber) at every loop header (futari-nx `tools/perf_codegen_post.py` pass 3). Check every Switch ELF with `tools/find_self_loops.py` (expected: only genuine guest `b .` halts). |
| Post-codegen passes run by hand | Futari | The build re-runs codegen whenever the manifest/tomls/xex change and silently dropped the direct-call and no-volatile passes (an NRO shipped without them). | APPEND the pass to the codegen custom command in app/CMakeLists.txt. |
| Direct calls + hooks added later | Futari, SA2 | Calls turned into `__imp__sub_X` before a hook existed bypass the new hook. | The pass turns direct calls back into hookable calls for newly hooked addresses (Futari) / run direct_calls `--undo` first (SA2). |
| Compile flags missing on the game-core / DLL targets | Futari, Banjo | `-ffp-contract=off -fno-strict-aliasing` reached only the main targets; game cores built with FMA contraction and strict aliasing (FP drift, aliasing bugs). | Set them on every target with generated code. |
| `-lvulkan` linking the wrong NVK | Banjo (found), Futari | The devkitPro toolchain puts `portlibs/switch/lib` (generic Mesa 26.2.1) ahead of the NVK SDK: every NRO shipped the generic driver, not nfsmw's 26.2.2. | Link `${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a` by full path; check `strings <elf> \| grep "Mesa 26"`. |
| `masseffect_native_deferred_recording` together with `masseffect_native_output_no_wait` | Futari | PC: ~100 ms stalls on GPU waits around copies, 23 fps instead of 60 ("[hitch] frame of 100 ms ... 1 GPU waits"). Either one alone is fine. | CAUSE (sa2-nx): the deferred worker's 5-20 us `sleep_for` naps round up to 1-15.6 ms on Windows. Futari: `ShortPause()` yields on non-Switch → PC 60 swaps/s, no stalls (2026-10-06). Still EXPERIMENTAL (console untested; see Banjo's hang row); console test config `futari_cpu_on_deferred.toml`. |
| `masseffect_native_deferred_recording` (+ `_fast`, `_update`) — **not pixel-identical** | Futari (found 2026-10-07; check every port that has it on) | Lockstep captures (vblank_per_swap + frame hashes labelled by VdSwap count, compared only on frames that reproduce across runs): the Mode Select banner's fade differed on frames 3180-3184 by up to 65 levels, differently in each of two deferred runs, while five runs without it (cpu-off included) agreed exactly. Looks like the deferred path is a frame late on some data during a fade. PC ring also slower (drains wait for the worker). | Explicit-only in Futari. Before enabling it anywhere, lockstep-compare a fade/transition with and without it. |
| `masseffect_native_dynamic_state` + `masseffect_native_dynamic_state3` (EDS1/2/3: blend, Z, stencil... as vkCmdSet*) — **not pixel-identical** | Futari (found 2026-10-07, PC) | Tempting in Futari: 95.8 % of its pipeline binds differ only in blend state (~160 binds/frame -> 7), and the PC in-run A/B gave ~8 % less ring work per gameplay draw; its own guard (FillFixedState) reported no MISMATCH and verify mode 0 differences. But on the Mode Select fade (frames 3179, 3180, 3182), where five runs without it agree exactly, it renders different images (as deferred recording does there); the menus are identical (431/431). (An earlier count of ~200 differing gameplay frames was confounded: gameplay frames are only ~47 % reproducible between runs.) | Off (code default). Do not enable from its self-check alone: lockstep-compare. |
| Lockstep comparisons on gameplay frames | Futari (2026-10-07) | Futari's gameplay frames are only ~47 % reproducible between two runs of the same build, and less between runs at different times (PC load shifts game timing): a few hundred "differences" in gameplay appeared for changes that are pixel-identical (math flags). | Judge on stretches several control runs reproduce exactly (menus, a fade), with control runs made at the same time. |
| Old `<port>.toml` on the SD card | Futari | A toml from an older build lacks the newer keys: they stayed at code defaults (mostly off) → 21 CPU enhancements silently off in a console run. | A key the config does not mention means ON (cvar source kDefault); log "(N not in the config, turned on)". |
| Pack storage short reads | SA2 (shared pack code) | `fsFileRead` short read treated as failure: a 16.5 MB .pak read as 0 bytes, re-read every 70 ms forever. | Loop on what fsFileRead returns (SA2 fix). |
| Renderer probing the NVK per-draw contract by its English name | Futari (all masseffect-lineage renderers) | The nfsmw driver exports `nvk_switch_dibujo` / `vk_switch_precargar_pipeline` (Spanish names); the renderer looked for `nvk_switch_draw` / `vk_switch_preload_pipeline` (weak → null): the per-draw NVK improvements never ran ("lacks the nvk_switch_draw contract"). | Accept both names (same layout, version 1). |
| Diagnostics keyed by `fmt::format` per draw | all masseffect-lineage renderers | "bounded draw extent" key built every draw until 32 distinct (never reached), EDRAM4 decline reasons and draw description formatted per draw: ~8 % of the ring thread. | Key by value / format at report time (`futari_ring_no_per_draw_copies`). |
| Vertex conversion read back from the upload buffer | all masseffect-lineage renderers (RepackWord1011 too) | The upload buffer has no CPU cache (NvMap without HOST_CACHED; write-combined on PC): read-modify-write there was 91 % of Futari's ring thread on PC. | Convert in a cached scratch buffer, write the upload buffer once. |
| Localized copies (tools/localize_native.py): one-line `if (..) return;` / calls in the generated code | SA2 | The copy rewrote `return;` as `SYNC; return;` without braces, so `if (cr6.eq) SYNC; return;` returned unconditionally (njTranslate, njMultiMatrix). | Braces around every synced call/return. The guard caught it on the first run (DIFFERENCE, switched off). |
| Localized copies: `__savegprlr_N` / `__restgprlr_N` not treated as calls | SA2 | They read and write ctx registers; without a sync around them the copy would save/restore stale values (it did not compile, undeclared). | Treat them as calls (sync + declare). |
| Guard comparing ALL registers on a native that calls other natives | SA2 | False DIFFERENCE: a checked nested memcpy runs the original (r4 advanced), an unchecked one the native (r4 untouched), so njPushMatrix's r4 depended on which nested call was checked. | Compare outputs + non-volatiles for natives that make calls; ALL registers only for leaves. Callers' liveness checked with call_site_liveness.py. |
| `masseffect_native_deferred_recording` | SA2 (seen too, without output_no_wait off) | PC: frames of 100-160 ms, swaps 600 → ~85 per 10 s. | Off (explicit only) in SA2 as in Futari. |
| `-lvulkan` linking the wrong NVK | SA2 (affected too) | Every SA2 NRO up to 2026-10-06 12:00 shipped Mesa 26.2.1; the console log said "lacks patch_nvk_set4". | Full-path link (same fix). |
| `masseffect_native_deferred_recording` (base switch only) | Banjo | PC: the game never left the green "LOADING" screen. Banjo reads rendered images back to the CPU hundreds of times per 10 s; every read-back drains the queue and waits for the worker: "3247 drains waiting for the worker, 9577 ms" per 10 s. With masseffect-nx's companion settings (naps 8, fast, batch 16, update) it passed but still waited 1.7 s per 10 s. | Off in Banjo's configs; explicit only. Reproduce with a save that continues into a game-world load. |
| Two or more pipeline-prewarm threads on the console | Banjo | NAK compiles allocate constantly; with 2 prewarm threads + the ring's own compiles, the ring spent 23 % of its samples in svcArbitrateLock (newlib's global malloc lock) and the game's loader starved (prewarm threads cannot go below priority 0x3B, the game's own). Marathon's logs show the same malloc serialization. | 1 prewarm thread (`banjo_prewarm_threads = 1`). |
| Holding pipeline-cache saves back during the prewarm | Banjo | A 3-minute rule while prewarming meant a 95 s console session saved nothing: every compile was lost (Switch apps are usually closed with HOME, which skips the final save). | Serialize the driver cache on the writer thread and save every 20-45 s. |
| Fast vertex-copy self-check comparing with a second read of guest memory | Banjo (shared masseffect code) | One false MISMATCH in millions of copies under the verify config: the game rewrote a dynamic vertex buffer between the copy and the check. | Run both paths on one snapshot of the source (`CopyVerticesFast`). |
| Texture fetch constant's exponent bias ignored by the renderer | Transformed (found; every masseffect-lineage renderer) | FXAA 360 binds its source with exponent biases -1 and -2 so that two fetches add up to their average; ignored, every edge FXAA blended came out twice as bright: 1-pixel white outlines and speckle (266 isolated bright pixels a frame in a race). The emulated GPU applies it. | Bias in bits 18-23 of the per-slot descriptor index, scaled in shader_common.h's tfetch helpers (`sasrt_native_texture_exp_bias`, 266 -> 9 specks a frame); renderer and shader package rebuilt together. |
| Lazy stencil fetch from tiles a 64-bit fixed-point color target wrote | Transformed (found; masseffect-lineage renderers) | The import of FP16-held k_16_16_16_16 tiles as stencil is impossible (only k_16_16_16_16_FLOAT has a raw view): 8 draws and 8 copies rejected at one transition (an intro skipped), the luminance chain never recovered: a whole race white. | Keep the depth image's own stencil instead of rejecting the draw (`sasrt_native_stencil_keep_64bpp`); on the hardware those bits are leftover color. |
| Shader package built from what one race drew | Transformed | Other tracks lost up to 99,000 shader loads per race (their draws skipped). | Harvest every loaded shader object by a guest-memory scan (`sasrt_dump_shaders_scan_s`) on each track: 250 -> 1,982 shaders, 0 unidentified on all 20 tracks. |
| Native guard comparing the host FPU control register whole | Transformed | x86's MXCSR holds sticky exception flags: a localized copy that skipped one operation on a denormal left the denormal flag clear -> false DIFFERENCE, hook switched off. | Compare only the guest-controlled bits (`FPSCRRegister::GuestMask`: rounding + flush). |
| `masseffect_native_fast_copy_verify` on every copy in verify mode | Transformed | The ring fell to 4 frames per 10 s in the intro. | The verify config checks the first 20,000 copies. |
| `non_volatile_as_local` with a split tail entered by **conditional** branches | Peggle 2 | RtlReAllocateHeap's tail 0x8231CEE4 runs its `__finally` funclet with the caller's r31; a scan that only looked at unconditional entries missed it: PC crash reading guest 0x60. | `share_registers = true` on the tail; count conditional branches into split pieces in the residue scan. |
| Enhancement defaults decided by VALUE (`HasNonDefaultValue`) | Peggle 2 | A toml line setting a key to its code default (e.g. `= false`) compares equal to the default, so Apply() switched it back on: individual offs (and `masseffect_renderer_native = false` on the Switch) were ignored. | Decide by source: `rex::cvar::GetFlagSource(key) == Source::kDefault`. |
| Localized copies of functions that make many calls | Transformed (PC A/B) | The copy writes back and reloads EVERY localized register at each call: for call-heavy functions that cost more than the locals saved. PC, same race, batch on vs off: 82754EC0 850 -> 1,538 samples, 8274C300 503 -> 1,060, 825E0F28 1,410 -> 1,931; game threads +13-18 % CPU per frame with all 36 copies. Leaves got faster (memcpy 784 -> 6, leaves -24..-69 %). | Transformed ships only leaves (+ functions whose only calls are the save/restore helpers). Worth an A/B in every port that ships call-heavy copies; a fix is a localizer that syncs only what a call can read or change. |
| `dform_disp_split` + a guard that logs or shadows guest stores/loads by rewriting `REX_STORE_U*` / `REX_LOAD_U*` | Peggle 2 (found 2026-10-07; check every port with a store-log or shadow guard) | With the D-form option the generated code stores through `REX_STORE_D_U8/16/32/64(reg, disp, value)` (Windows: expands to `REX_STORE_U*((u32)(r)+(d))`; Switch: `REX_ADDR_D` directly). The guard's rewrite only matched `REX_STORE_U*(`, so those stores were not logged: the rollback was incomplete, the original ran on memory the copy had changed, verify mode reported 7 false differences in Wwise leaves (counters off by exactly 2x), and every guard-checked call in normal runs left the copy's stores in guest memory. | Rewrite `REX_STORE_D_U*` (and `REX_LOAD_D_U*` for a shadow guard) too (peggle2-nx `PEGGLE2_LOG_STORE_D_U*`); verify 0 differences over 129.7 M checks after the fix. |
| Lockstep reference captures made before a run that SAVED progress | Peggle 2 (found 2026-10-07; every port with lockstep captures) | An 8-minute route saved past the adventure intro; every later capture skipped the intro and matched only 50 of 123 reproducible frames against the older baselines, although verify mode was clean (0 differences over 111.9 M checks). Not a code difference. | Each test run gets `user_data_root` (env `REX_USER_DATA_ROOT`) = a fresh copy of a fixed seed folder; references and tests use the same seed. With an empty seed Peggle's defaults matched 122/123 again. The user's Documents save is never touched. |
| sa2-nx `tools/non_argument_audit.py`: element-write runs | Peggle 2 (2026-10-07) | After a run of element writes covering one vector register (vlogefp128 v60), `compute_ud` resumed past the statements it had skipped, which can hold the next register's run (vlogefp128 v59): those writes then counted as partial writes = reads before written. 9 false reports in Peggle (vlogefp, vsraw). False alarms only, never a missed case. | Track the register's last element-write index, mark only `range(i, last + 1)`, resume at `last + 1` (peggle2-nx's copy). |
| sa2-nx native Direct3D layer, verify: device bytes inside a compared region | Peggle 2 (2026-10-07) | `CompareShadow` flagged BeginVertices device+12619 once per frame (native N, original 0). Direct3D's own draws bind a vertex shader object embedded in the device; the shader load updates its binding state through the shader pointer, so the shadow run's write lands in the real device, not the copy. | Skip device bytes covered by one of `s.regions` (the region comparison checks them). Then verify 116.2 M checks, 0 differences. |
| sa2-nx native Direct3D layer: native Clear (`sa2_api_clears`) | Peggle 2 (2026-10-07; cause not found) | Native draws are fine, but with the native Clear the game stops presenting ~20 s in: 0 packets and 0 swaps per 10 s, the game thread loops in D3D's GPU wait with the read-pointer write-back creeping, device+11088 far ahead. Verify compared all 497 native clears (ops, device, regions, vertices): 0 differences. Ruled out: lost records (0), the inline-data allocator overlapping the marker (vertices always past the segment end), tile-replay address collisions (0). | Explicit-only in Peggle (it clears ~0.5 times a frame). Bisect by key before trusting verify alone for Clear. |
<!-- END KNOWN ISSUES -->

---

## General optimisations (apply to every port of this lineage)

<!-- BEGIN GENERAL (append rows, any port) -->
| Optimisation | Key / where | Effect (where measured) | Notes |
|---|---|---|---|
| Codegen options: CR/XER/CTR/reserved as locals, skip MSR, no LR stores ("diet"), split D-form, inline fctiwz | perf_overrides.toml (build-time A/B) | masseffect-nx: diet −10 % main thread | Needs share_registers for setjmp callers. |
| Direct calls between recompiled functions | post-codegen pass | lets GCC inline; ~178k calls in Futari | Hooks must stay `sub_X` calls (see known issues). |
| Guest memory without `volatile` + loop barriers | post-codegen passes 2+3 | masseffect-nx +15-20 % | Barriers are mandatory (known issues). |
| `non_volatile_as_local` / `non_argument_as_local` | codegen (not yet in Futari) | masseffect-nx +9 % | Needs share_registers on split fragments AND their parents (`scan_caller_register_reads.py --with-callers`). |
| D3D GPU-wait spin → sleep until the ring advances | `masseffect_wait_blocking_ring` + a hook on the XDK `db16cyc` spin (offsets differ per game) | Futari PC: game thread 96 % → 8 %; console: mainThr 96 % → 59 % (with other fixes) | Find the function with 8× `db16cyc` in a 4-count ctr loop. PC: translate guest addresses like REX_RAW_ADDR (+0x1000 ≥ 0xE0000000). |
| Native XDK memcpy (host memcpy, overlap → original) | `<port>_native_memcpy` | Futari: 40 M verified calls, 0 diff | Same 290 instructions in every module of Futari. |
| Missing config key = ON | `Apply()` in the enhancement table | protects against old tomls | Mark risky rows explicit-only. |
| CPU sampler switched by config | `switch_profile_stacks` cvar | profiles always have samples | No flag file needed. |
| PC sampling profiler with callers + lines | `<port>_pc_profile_s` (futari_pc_profiler.cpp) | found every ring hot spot above | Suspend/GetThreadContext; stack scan for return addresses. |
| PC swap limiter for console-like profiles | `<port>_pc_swap_limit_hz = 60` | PC native renderer otherwise runs uncapped (~1800 fps) | Test runs only. |
| Localized copies of hot guest functions | tools/localize_native.py (SA2, Futari) | exact by construction; faster on ARM (registers in locals) | Futari adds a record-and-replay guard that also covers functions calling localized leaf callees. |
| Native-function framework | futari_native.h (Site: key + code hash + verify) | — | Code hash of the guest bytes per site; verify counted in `[verify]`. |
| Natives must not write registers the original leaves alone (MSVC interprocedural register allocation) | `tools/call_site_liveness.py` (SA2, from masseffect-nx) | SA2: callers of njCalcPoint read r4-r10 after the call | Leaves: reproduce every register the original writes; callers: write only outputs. |
| Rank PC-hot functions by their AArch64 cost | `tools/arm_hot_functions.py` (SA2) | PC busy share × FPCR switches, byte swaps, calls from the Switch ELF | Symbolize with the exe that recorded the profile. SA2: of 46,307 FPCR mode-switch points only 196 enter flush mode, so FPCR is cheap there. |
| `-O0` for the generated `<project>_register.cpp` (Switch) | app/CMakeLists.txt (from sasrt-nx) | SA2: build 16-20 → 8 min | One startup function; no run-time effect. |
| Pack storage: continue after short `fsFileRead` reads | sdk pack_storage_switch.cpp (SA2) | — | Also a host bounce buffer if a direct read into guest memory fails. |
| Shader library with compile-time constants (shader_common.h): `MASSEFFECT_UBO` true at compile time, tfetch2D 1/size unconditional, texture signs + remapInput via select() | build-time (library file A/B) | Banjo: PS SPIR-V −47 %, blocks −72 %, library 62 → 35 MB; cold compile on a PC AMD iGPU 3-4.5x faster | Output-identical: the runtime always took those paths (force use_ubo_ / inv_tex_size_ on). HLSL 2021 `?:` on scalars branches: use select(). |
| First-boot pipeline prewarm list recorded on PC | `tools/make_prewarm_list.py` (Banjo) | Banjo console: the list held 63 of the first 64 pipelines the console created, in the same order | Strip the PC driver-cache part; dedupe by shader fingerprint (library numbers change on rebuilds). |
| NVK per-stage shader cache (`disable_lto` behind NVK_SWITCH_STAGE_CACHE) | driver rebuild + `masseffect_nvk_cache_per_stage` (Banjo: D:\Banjo kazooie port\mesa-banjo) | masseffect-nx: cold pipeline 137 → 81 ms | In the nfsmw driver the linked compile only drops unread VS outputs, so the image is the same. |
| Pipeline-cache save off the ring, every 20-45 s | writer thread calls vkGetPipelineCacheData (Banjo) | PC: 10-27 MB saves in 9-48 ms, nothing on the ring | Unleashed/Marathon save at loading-screen ends instead; Banjo's first boot can last minutes, so periodic. |
| Texture exponent bias (and, experimental, the integer number format) in the tfetch helpers | shader_common.h + renderer slot bits 18-23 / 12-16 (sasrt-nx) | exact FXAA 360 (and any biased fetch) | The integer format is off: Transformed's GI volumes come out noisy with it (3D data probably not decoded right). |
| Shader harvest by a guest-memory scan of loaded shader objects | `<port>_dump_shaders_scan_s` (sasrt-nx: sasrt_me_hooks.cpp) | one visit of a track gives all its shaders (8,555 objects in a race) | PC only (reads committed heap regions; freed physical pages are decommitted on the host). |
| Shadow guard for localized leaf copies | tools/localize_native.py emits Shadow(): stores kept aside, loads read them first (sasrt-nx) | no hand-written write ranges, no rollback; every register + every stored byte compared | Leaves whose memory goes through the REX_LOAD/STORE macros only; loop barriers inside every copy (plain memory). |
| Lazy-stencil fallback for 64-bit fixed-point color sources | `sasrt_native_stencil_keep_64bpp` | no rejected draws at such transitions | masseffect-lineage renderers. |
| Store-log guard for localized leaf copies | tools/localize_native.py emits `NativeLogged` (peggle2-nx): every guest store (REX_STORE_*, stvx, dcbz) records the bytes it overwrites | no hand-written write ranges; the guard rolls the log back, runs the original, compares every register + every written byte. Peggle 2 PC: 90.7 M checks, 0 differences | Leaves only (no calls); the same tool puts a loop barrier at every loop header and writes the hook lines with each function's code hash, so a new native is one line in a list. |
| call_site_liveness.py: ignore value-free register idioms | `subfe/subfc rD,rA,rA` (result from the carry: ~rA + rA is all ones) and `mr rX,rX` / `or rX,rX,rX` (no-op / priority hints) name a register without reading its value (peggle2-nx patch) | Peggle 2: all 4 "reads" after memcpy were these; with them ignored, 1,083 memcpy and 665 memset call sites are clean | Otherwise a native CRT routine looks unsafe at sites that are fine. |
| Frame-locked guest clock for lockstep captures of clock-driven games | SDK cvar `clock_per_swap_hz` (sdk/src/core/clock.cpp + `Clock::NotifyGuestSwap` in VdSwap; peggle2-nx): the guest clock (mftb, KeQueryPerformanceCounter, KeQuerySystemTime) advances 1/N s per swap; a thread's k-th read in a frame = frame start + k us; `clock_per_swap_timeout_ms` (200) without a swap = one frame. With `vblank_per_swap` + new `vblank_per_swap_count` (2 for 30 fps games) | Peggle 2 PC: not enough on its own (host-timed loading/timers remain), but the remaining differences are reproducible-frame edges | Test settings only (off by default). `tools/compare_hashes.py TEST BASE1 BASE2` (peggle2-nx): only images identical in every baseline at the same game frame count, looked up at any offset in the test run |
| Vertex dedupe that never hits | Check the `C6 vertex dedupe (10 s)` line (sa2-nx round 6 prints it): Peggle 2 had 0 hits in every window while fingerprinting ~19 MB/10 s. peggle2-nx's `peggle2_cpu_ring_dedupe_adaptive` (probe 32 frames, rest 256) cut it to 2 MB | Peggle 2 PC: fingerprinting -90 % | Exact by construction (no reuse = the same bytes copied again) |
| `lr_keep_callees` (generator option, peggle2-nx) | `perf_overrides.toml`: `lr_keep_callees = [0x...]`; config.h/.cpp + one line in `build_bl` | lets `elide_lr_stores` ship in a port whose hooks read `ctx.lr` as their caller (heap tracker, throw logger): every `bl` to a listed function keeps its store | Peggle 2: `ctx.lr` stores 148,273 -> 10,681. Guest stack walks and profiler caller attribution still lose depth. |
| `non_argument_as_local` audit checklist (from sa2-nx's) | `tools/non_argument_audit.py` without and `--with-option` | Peggle 2: `ctx.r11` uses 549,939 -> 13,488; NRO -2.9 MB | Look for unwind funclets that are NOT share_registers (start `addi r31,r12,-N`, reached only via `_CallSettingFrame`'s blrl): Peggle had 177, put in `non_argument_as_local_keep` with the stack probe, `_CallSettingFrame` and the continuation jump. Pass error exits and thread exits as `--noreturn`. |
<!-- END GENERAL -->

---

<!-- BEGIN MUSHIHIMESAMA -->
## Mushihimesama Futari Ver 1.5 (futari-nx) — D:\mushihemesama\futari-nx

Structure: default.xex front end + 8 game-core DLLs (palm/reco/hirow/kiniro × normal/abnormal) all at 0x88000000;
natives for cores are compiled into each core target. Profile tools: `tools/switch_hot_functions.py` (console),
`tools/pc_hot_functions.py` + `futari_pc_profile_s` (PC). Verify: `configs/futari_cpu_verify.toml` ("[verify] ... |
differences: 0").

### Game-specific (guest code)
| # | Optimisation | Key | Hot spot it removes | Verified | Status |
|---|---|---|---|---|---|
| 1 | Frame pacing sleeps to the next vblank instead of spinning on the frame counter (mid-asm hooks at 8216B244 / 8216B2E4 in sub_8216B148 + hook on the vblank callback sub_8216AD38) | `futari_frame_wait_sleep` | console: 78.6 % of the game thread's busy time | PC: 7.4 s of 10 asleep, 60 swaps/s | pending console (build F) |
| 2 | LZSS decompressor of the game cores as native code (sub_8813A120 + 7 copies) | `futari_native_lzss` | console: 27 % of gameMainThr | PC verify 398/0 | pending console (build F) |
| 3 | Localized copies of the audio mixer's 8 hottest functions (821536C0, 82127390, 821528D0, 82143450, 8212BE20, 821268F8, 82127780, 82129348) | `futari_native_hot_<ADDR>` | console: mixer thread top 8 (≈60 % of it) | PC guard: 12k-39k calls each, 0 diff; planted-bug test caught on the 1st call | pending console (build G) |
| 4 | D3D GPU-wait sleep (default 820A4590 + core copies) | `masseffect_wait_blocking_ring` | console: game thread 96 % (with #1) | console: ~2 s of 10 asleep | console-verified |
| 5 | Native XDK memcpy (all 9 modules) | `futari_native_memcpy` | — | PC verify 40 M+/0 | console-verified (runs) |

### Renderer (host code, ring thread)
| # | Optimisation | Key | Effect | Status |
|---|---|---|---|---|
| 6 | k_32* sprite vertex conversion in cached scratch memory, linear NEON pass | `futari_native_repack_fast` | PC: 91 % of the ring → gone; console ring 89 % → 31 % (with #7, #8) | console-verified |
| 7 | No register-file copy per rectangle list; diagnostics keyed by value | `futari_ring_no_per_draw_copies` | PC: ~8 % + allocations | console-verified |
| 8 | Rectangle expansion buffers reused (no malloc/free per sprite draw) | `futari_ring_no_per_draw_copies` | console: malloc/free/memalign + newlib lock ≈ 14 % of the ring | pending console (build F) |
| 9 | NVK per-draw contract found under the nfsmw driver's Spanish names | (masseffect_native_nvk_* keys) | driver per-draw work (descriptor binds, dynamic state, cbufs) | pending console (build F) |
| 10 | masseffect-nx ring set (fast pair, raw microcode, PM4 NEON runs, flat identity, compare_fast, fast copy, indices cache, notify_no_lock, fingerprint mode 1, ...) | 40+ `masseffect_*` keys | console with all on: 51-60 fps → 61-63 fps (with #4, #6, #7) | console-verified (self-checks 0) |
| 11 | Direct renderer step 1 (sa2-nx's design): the state packets each BeginVertices / BeginIndexedVertices writes become a host record + one NOP marker; the ring applies the record with the same register functions. Hooks: default.xex sub_8209D800 / sub_8209DCE0 and each core's BeginVertices (exe exports). Futari specifics: the draw packet's end is device+13652 (EndVertices copies it to +48 later); records held until XE_SWAP (tiling replays find them); SET_CONSTANT2 / SET_SHADER_CONSTANTS carried | `futari_cpu_direct_state` | 98.8 % of hooked calls, 82-87 % of ring draws converted (the rest: D3D Resolve / quad helper). PC ring time: no measurable gain (in-run A/B, on/off every 2 s: 12.33 vs 12.27 us/draw over 1.3 M draws each; parsing is ~8 % of the PC ring); console A/B pending (`futari_test_direct_state_ab.toml`). Ring-side re-check of every region at apply time + the 3 words written back (sasrt-nx). Verify: 1.23 M records re-encoded identical, every marker found, 2.78 M register runs equal to the PM4 path; lockstep frames on vs off 699/699 identical | PC-verified; console A/B config `futari_test_direct_state_off.toml` |
| 13 | Rectangle-list expansion: a sprite's three corners written once, the 18-stride block doubled (6 copies instead of 18 small memcpy calls; the hottest line of DrawsVulkanImpl::Draw on the PC line profile) | `futari_ring_rectangle_fast` | ~2-3 % of the PC ring | PC verify 809,836 / 0 differences |
| 14 | In-run A/B of the direct state (banjo-nx's method: on/off every 2 s in one run, ring work split by phase) | `futari_direct_state_ab_s` (test) | PC: on 12.33 vs off 12.27 us/draw (1.3 M draws each): no PC gain | console config `futari_test_direct_state_ab.toml` |
| 15 | EDRAM draw memo (sa2-nx's): a draw with the same render-target registers as the previous one reuses its EDRAM preparation when the full path would do no work | `futari_ring_edram_memo` | 99.3 % hits; verify 1.57 M / 0; PC cycles A/B ~-1 % (console EDRAM ~6 % of the ring) | PC-verified |
| 16 | Rectangle layout memo: consecutive sprite draws with the same source layout (by content) and VS keep the expanded layout (no copy/rebuild/XXH3/set inserts per draw) | `futari_ring_rectangle_fast` | with #13: PC cycles A/B ~-6 % of the ring per draw; verify 1.36 M / 0 | PC-verified |
| 17 | Measurement: in-run A/B counting the ring thread's CPU cycles (QueryThreadCycleTime on the PC: immune to other processes' load), draw census, console sampler pc/lr pairs ("callers of" mutexes/malloc) | `futari_ab_s` / `futari_ab_key`, `futari_draw_census` | — | tooling |
| 18 | Always-failing / never-used ring work removed: PairDraw lookups without any Draw* record; vertex-dedupe fingerprints for repacked bindings and a dedupe that never hits (stops after 100 k queries < 0.1 % hits); extent-estimator runs for a VS whose microcode it already rejected (memo per VS load) | `futari_ring_pairing_skip`, `futari_ring_dedupe_lean`, `futari_ring_extent_memo` | console E: PairDraw 1.7 %, Estimate 1.8 % + interpreter; Futari skips ~25 k estimator runs/s; verify 0 differences | PC-verified |
| 19 | `-fno-math-errno -fno-trapping-math` on the generated code (masseffect-nx ships it) | CMake `FUTARI_GENERATED_MATH_FLAGS` | no errno branch / libm call after sqrt; fewer FP trap constraints | lockstep: menus 426/427 and fade 8/8 identical; verify 0 |
| 12 | Lockstep test tooling: sa2-nx's `vblank_per_swap` (SDK), `futari_video_frame` / `futari_video_every` / `futari_video_hashes` (frame hashes labelled by game frame), `tools/compare_frames.py` | — | Futari is not fully deterministic (two identical cpu-off runs share ~700 of 3000 frames): compare only labels that reproduce across runs of each config | tooling |

### Build / platform
| Optimisation | Effect | Status |
|---|---|---|
| Codegen perf options + direct calls + no-volatile + loop barriers (build D+) | NRO 96 → 71.5 MB | console-verified (boots, plays) |
| nfsmw NVK driver (Mesa 26.2.2) linked by full path | set 4 by differences active (33 % fewer cbuf flushes) | console-verified |
| Handheld GPU 460.8 MHz | `futari_handheld_gpu_boost` | console-verified |

### Known issues (Futari)
- Deferred recording + output_no_wait: PC stalls (see the shared table); deferred recording is experimental/off.
- Deferred recording is NOT pixel-identical (Mode Select fade, frames 3180-3184; see the shared table): stays off.
- cpu-off presents one frame later than the defaults (lockstep): frame-by-label comparisons across those two configs
  need a ±1 offset.
- Builds B and C (non-volatile loads without loop barriers) hang at boot — do not use.
- The 3 mixer functions that make indirect calls (82153CB8, 821493E0, 8211F330) are not localized yet (no guard).
<!-- END MUSHIHIMESAMA -->

---

<!-- BEGIN SONIC ADVENTURE 2 -->
## Sonic Adventure 2 (sa2-nx) — D:\sonic adventure 2\sa2-nx

**Structure.** default.xex plus one stage DLL (gd/DLL/xbox 360/Data_DLL.xex). On the Switch the DLL is a static module.

**Framework.** `app/src/cpu/sa2_enhancements.{h,cpp}`:
- `SA2_ENHANCEMENT` keys under the masters `sa2_cpu_enhancements` / `sa2_noncpu_enhancements`;
- a `kExternal` table with the on and off values of the renderer's own cvars (a key missing from the toml is turned on);
- the `[enh]` startup line, which names the build and the codegen flavour;
- `[enh] uses` counters every 10 s;
- `sa2_cpu_verify`, which reports in the `[verify]` line.

**Configs.** `tools/make_enhancement_configs.py`: cpu_on and cpu_off (profiler on in both), noncpu_off, all_off, verify,
profile, and test configs.

**Notes.** All in `docs/enhancements.md`.

**Console baseline** (classic NRO, log `sa2_001 (2)`, City Escape):

| CPU clock | FPS in gameplay | Ring thread | Game thread |
|---|---|---|---|
| 1020 MHz | 28-50 | 60-82 % of a core | 50-76 % |
| 2397 MHz | 58-60 | | |

The game is CPU-bound, and it ran before the ring set below was on.

### Game-specific (guest code)

| # | Optimisation | Key | Hot spot | Verified | Status |
|---|---|---|---|---|---|
| 1 | D3D GPU-wait iteration sub_8254F400 (8× db16cyc, ctr 4) sleeps until ring progress / interrupt; per-thread progress snapshot taken after the original returns | `sa2_cpu_gpu_wait_sleep`, `sa2_gpu_wait_max_us` | PC: ~70 % of the game thread spinning | PC: game thread 1.01 → 0.55 cores at 60 fps | PC-verified, pending console measurement (runs) |
| 2 | Native CRT memcpy / memset / memmove (sub_827F05C0 / 827EFEA0 / 827EF8B0): host libc; overlaps the original would smear are declined | `sa2_cpu_native_memcpy/memset/memmove` | PC: memcpy 6.1 % of the busy game thread | guard + verify mode: 115-133 M calls, 0 diff; PC −35 % time in memcpy | PC-verified (console: runs, guard 0 diff) |
| 3 | Localized copies of the Ninja matrix library: njCalcPoint 82146ED8, njMultiMatrix 82146F60, njTranslate 82147AC0, njPushMatrix 82147DA8, njRotateX/Y/Z 82147B38/C08/CD8, plus a std::map lower bound 8213EC28 | `sa2_cpu_native_calc_point`, `_multi_matrix`, `_translate`, `_push_matrix`, `_rotate_x/y/z`, `_map_lower_bound` | PC: njCalcPoint 4.2 %, lower bound 3.0 %, njMultiMatrix 1.9 % | guard (all registers for leaves, outputs for callers) + verify mode, 0 diff; PC −25 % (njCalcPoint, njMultiMatrix) | PC-verified (console: runs, guard 0 diff) |
| 4 | Localized copies of the next 15 hot functions (8213A4D8, 82139DA0, 8214CF10, 8214E130, 82555CC8, 821BC520, 82549178, 82960CE0, 82141420, 82555B70, 82960928, 8295F9C0, 82552408, 82960808, 8214D6D8); they call others, so unguarded | `sa2_cpu_native_sub_<ADDR>` | PC: 1.3-4.5 % each | PC lockstep capture: all gameplay frames identical, millions of calls each; PC time per function unchanged or higher (sync cost at calls, as sasrt-nx measured) | experimental (off; `configs/sa2_test_new_natives_on.toml` A/B) |
| 5 | Next batch, ranked by `tools/arm_hot_functions.py` (PC profile + AArch64 code in the Switch ELF): 82140C98, 8214A050 (njSin), 8216B088, 8214B520, 8251D2A0, 8213D1F8, 82146E48, 8214ACA8, 821B0978, 82141240, 8216E0D0, 821BA660, 821B4D08, 821B5838, 8217E338 | — | — | — | not started; waits for the console A/B and profile |

### Renderer (host code, ring thread)

| # | Optimisation | Key | Effect | Status |
|---|---|---|---|---|
| 6 | masseffect-nx ring set: flat identity, fast pair, raw microcode, PM4 fast runs, compare_fast, fast copy, fingerprint mode 1, dedupe max fingerprint 16384, indices cache, pipelines prewarm | 10 `masseffect_native_*` keys in kExternal | PC lockstep capture: all gameplay frames identical; verify mode 0 diff | PC-verified, pending console |
| 7 | Ring progress notify without the mutex unless someone sleeps | `sa2_cpu_ring_notify_lockfree` | — | PC-verified |
| 8 | EDRAM transfer-trace text formatted only when logged | `sa2_cpu_lazy_trace_text` | PC: ~4 % of the ring | PC-verified |
| 9 | Deferred recording | `masseffect_native_deferred_recording` | PC: frames of 100-160 ms | experimental (off, explicit only) |
| 10 | GPU optimisations: depth + color-0 clear redirect, constants dedupe, narrow pass end, ZCULL 1x depth | `sa2_native_edram4_clear_color0`, `sa2_native_constants_dedupe`, `sa2_native_pass_narrow_end`, `sa2_native_zcull_1x_depth` (off) | PC: pixel-identical | pending console (dedupe is a suspect for the truck-light flicker, see below) |

### Build / platform

| Optimisation | Effect | Status |
|---|---|---|
| Optimised codegen flavour (`tools\codegen.ps1 -Flavor optimised`): CR/CTR/XER/reserved locals, skip_msr, split D-form, inline fctiwz + no-volatile + loop barriers (20,331 headers) + direct calls (91,077); `tools/post_codegen.py`, appended to the build's codegen command | NRO 66.5 → 55.2 MB; PC verify 128 M/0 and lockstep frames identical; self-loops 40 → 15 (all genuine guest halts) | pending console (`sa2_optimised.nro`) |
| nfsmw NVK (Mesa 26.2.2) linked by full path | driver paths on: set 4 by differences, NVK per draw (console log: requested and active) | console: runs |
| Handheld GPU 460.8 MHz | `sa2_gpu_boost` | console: applied (the memory clock was reimposed by firmware) |
| `-O0` for the generated sa2_register.cpp on the Switch | build 16-20 → 8 min (the file alone took 771-927 s at -O3) | done |
| `-ffp-contract=off -fno-strict-aliasing` on the stage DLL target too (was missing) and on the PC | FP and aliasing behaviour now match the 360 | done |
| CPU sampler switched by config (`switch_profile_stacks`); every sampled pc with its count | console profiles on A/B runs | pending console |

### Known issues (SA2)

- Truck headlight glow flickers in City Escape on the console (first run with the nfsmw driver). Suspects: constants
  dedupe (#10) and the driver's set-4-by-differences path. Test configs: `sa2_test_dedupe_off.toml`,
  `sa2_test_driver_paths_off.toml`. Open.
- The first optimised NRO (no-volatile without loop barriers) must not be used: 40 branch-to-self loops, including one
  in Direct3D. Replaced.
<!-- END SONIC ADVENTURE 2 -->

---

<!-- BEGIN BANJO -->
## Banjo-Kazooie: Nuts & Bolts (banjo-nx) — D:\Banjo kazooie port\banjo-nx

Structure: one default.xex (renut), native renderer from masseffect-nx. Enhancement table + masters:
`app/src/cpu/banjo_enhancements.cpp` (`banjo_cpu_enhancements`, `banjo_noncpu_enhancements`; keys a config does not set
are turned on, deferred recording excluded). Verify: `banjo_cpu_verify` / `dist/switch/configs/banjo_cpu_verify.toml`
("[verify] ... differences: 0"). Configs: `tools/make_enhancement_configs.py`. Docs: `docs/enhancements.md`.
Console status so far: boots to the title; the first boot is compile-bound (jiggy loading screen); one console run in
verify mode gave 0 differences. No steady-state console profile yet.

### Game-specific (guest code)
| # | Optimisation | Key | Hot spot it removes | Verified | Status |
|---|---|---|---|---|---|
| 1 | Native XAudio reverb `princeton_digital::stereo_room_t<float>::process` (0x826683D0, ~1630 lines, NEON/SSE via vector extensions; fmadds as double fma rounded to float, unroll-position contraction reproduced; guards fall back to the original) | `banjo_native_reverb` (+ `banjo_native_reverb_verify`) | console: ~25 % of a core, all the time | standalone diff test 36,000 calls / 0; PC verify 18,000 calls / 0; console verify 20,500 calls / 0 (NaN payload bits may differ, NaN-ness identical) | PC ~12-14x faster; console perf pending |
| 2 | D3D GPU-wait sleep: hook on `sub_8264DCD0` (the XDK db16cyc check, device +10896 read-pointer word, +10941 hung bit 0x02) covering all six wait loops (ring space sub_8228F558, Swap sub_82287590, fences); first check of each wait never sleeps (Swap tests its condition after the check) | `banjo_wait_blocking_ring` | console: 3 game threads at ~55 % each spinning while the ring compiled | PC A/B: no fps loss (257-261 vs 251 swaps/10 s) | console: threads asleep in the hook (profile), perf pending |

### Renderer (host code, ring thread)
| # | Optimisation | Key | Effect | Status |
|---|---|---|---|---|
| 3 | masseffect-nx ring set (flat identity, fast pair, raw microcode, PM4 runs, compare_fast, fast copy, fingerprint mode 1, dedupe 16384, indices cache, notify_no_lock) | `masseffect_native_*` (defaulted on when the config omits them) | not measured on console yet | PC verify: 5.6 M identity / 1.2 M pairing / 0.3 M raw / 3.4 M PM4 checks, 0 diff; console verify run 0 diff |
| 4 | k_10_11_11 vertex repack in a cached scratch buffer (was read-modify-write in the uncached upload buffer) | `banjo_repack_cached` | — | PC-verified (same bytes by construction) |
| 5 | No register-file copy per rectangle list | `banjo_ring_registers_in_place` | — | PC-verified |
| 6 | Diagnostics keyed by value / formatted at report time (extent log key, EDRAM4 decline reasons, transfer-trace description) | (not switchable: log bookkeeping) | — | PC-verified |
| 7 | Measurement-only work off for play: texture-reuse fingerprints (ring), per-category GPU timestamps (~60 flushes/frame) | `masseffect_native_diag_reuse=false`, `masseffect_gpu_marks_categories=false` | — | PC-verified; on in the profile configs |
| 8 | Fast vertex-copy self-check runs both paths on one snapshot of the source (the old check re-read guest memory and gave a false MISMATCH when the game rewrote a dynamic buffer in between) | (part of `masseffect_native_fast_copy`) | — | PC verify 0 diff |

### Pipeline compile (first boot) — non-CPU
| # | Optimisation | Key | Effect | Status |
|---|---|---|---|---|
| 9 | Shader library with compile-time constants: `MASSEFFECT_UBO` = true, tfetch2D always 1/size from constants, texture signs and remapInput with select() instead of per-component branches (shader_common.h) | build-time (A/B by library file) | PS SPIR-V 18,668 → 9,928 words (−47 %), blocks 620 → 173 (−72 %); library 62 → 35 MB; cold compile on PC AMD iGPU ~310 → ~100 ms per pipeline (3-4.5x) | PC-verified (videos: old vs new no worse than old vs old); console compile time pending |
| 10 | Mesa disk cache off (MESA_SHADER_CACHE_DISABLE in OnPreSetup) | `banjo_mesa_disk_cache_off` | removed the foz_read_entry lock wait seen in console stall dumps | pending console |
| 11 | Pipeline prewarm from a first-boot list recorded on PC (keys + layouts + container fingerprints are host-independent; list dedup by fingerprint, `tools/make_prewarm_list.py`), 1 lowest-priority thread | `masseffect_native_pipelines_prewarm`, `banjo_prewarm_threads` | console: list matched 63 of the first 64 console pipelines | pending console (1 thread) |
| 12 | Driver cache serialized on the writer thread, saved every 20-45 s (was on the ring; and a 3-minute rule during prewarm lost a whole 95 s session) | (save cadence) | PC: saves of 10-27 MB in 9-48 ms off the ring | PC-verified |
| 13 | Per-stage shader cache in NVK (`dev->vk.disable_lto` behind NVK_SWITCH_STAGE_CACHE, from masseffect-nx's Mesa patch; nfsmw 26.2.2 rebuilt by hand into D:\Banjo kazooie port\mesa-banjo) | `masseffect_nvk_cache_per_stage` | masseffect-nx: cold pipeline 137 → 81 ms. In this driver the linked compile only drops unread VS outputs, so the image is the same | pending console |

### Build / platform
| Optimisation | Effect | Status |
|---|---|---|
| nfsmw NVK driver (Mesa 26.2.2) linked by full path (sdk/cmake/rexglue_switch.cmake) | every earlier Banjo NRO had the generic 26.2.1 | pending console |
| `-ffp-contract=off -fno-strict-aliasing` on renut + renut_recomp | console FP now matches PC/360 (correctness, not speed); NRO 78.8 → 80.3 MB | built, pending console |
| Switch profiler: every code line with its raw count (4000/thread), link map, `switch_profile_stacks` cvar | — | built |

### Known issues (Banjo)
- Deferred recording: kept the game on the "LOADING" screen indefinitely on PC (see the shared table); off.
- Two prewarm threads: on the console they mostly queued on newlib's global malloc lock together with the ring's own compiles and slowed the game's loader; 1 thread now.
- Not yet ported: the Spanish-named NVK per-draw contract (`nvk_switch_dibujo`), codegen perf options / direct calls / no-volatile (+ loop barriers).
<!-- END BANJO -->

---

<!-- BEGIN PEGGLE 2 -->
## Peggle 2 (peggle2-nx) — D:\peggle 2\peggle2-nx

Structure: default.xex only (XBLA, 34,045 functions; Wwise audio engine and Bink statically linked). PC: title to
gameplay with the native renderer. **Console: not running yet** — a Switch-only guest-heap corruption ~4 s into boot
(a live block handed out twice; under diagnosis with a heap tracker), so nothing below is console-verified. Profile
tools: `peggle2_pc_profile_s` + `tools/pc_hot_functions.py` (PC), `switch_profile_stacks` / the flag +
`tools/switch_hot_functions.py --map out/nx/peggle2.map` (console). Verify: `app/configs/peggle2_cpu_verify.toml`
(renderer `[verify]` + natives `[hot]` lines). Configs from `tools/make_enhancement_configs.py`; every key in the
`[enhancements]` startup line; missing keys = Peggle 2's default (by cvar source).

### Game-specific (guest code)
| # | Optimisation | Key | Hot spot it removes | Verified | Status |
|---|---|---|---|---|---|
| 1 | D3D GPU-wait sleep: hook on sub_8261D378 (the XDK `db16cyc` spin) sleeps on ring progress while the device's read-pointer word equals the wait value | `masseffect_wait_blocking_ring` | PC: without it the game spins through the wait ~20 M times/s | PC: 1,100-3,100 checks per 10 s, 4-9 s of 10 asleep; the game's own check decides afterwards (cannot change a result) | pending console |
| 2 | Native XDK memcpy / memset (sub_825F0AE0 1160 B, sub_825F10B0 160 B: byte-identical to SA2's, natives taken from sa2-nx) | `peggle2_cpu_native_memcpy`, `peggle2_cpu_native_memset` | PC gameplay: memcpy 2.1 % of the main thread | PC guard + verify mode (inside #3's count, 0 diff) | PC-verified |
| 3 | Localized copies, batch 1: 32 functions (20 main-thread game functions, 12 Wwise audio-thread functions incl. the mixer sub_825BE6A0 = 17.7 % of that thread). 13 leaves guarded with a **store log** (generated `NativeLogged`: each guest store records the bytes it overwrites; the guard rolls back, runs the original, compares every register + every written byte; no hand-written ranges); 19 that call others run unguarded | `peggle2_cpu_native_sub_<ADDR>` + `peggle2_cpu_natives` (all) | PC gameplay profile: ~38 % of the main thread's busy samples, ~42 % of the audio thread's. **PC effect (CPU time, same 2 min of gameplay, off -> on): main thread 43.8 -> 34.7 s (-21 %), audio thread 30.2 -> 23.6 s (-22 %)** | PC verify mode: **90.7 M checks, 0 differences** (130 s gameplay); default guard 37 k checks, 0 diff; frame A/B unusable (two runs WITHOUT natives differ as much: Peggle's gameplay is not frame-deterministic on the PC), so the unguarded ones are exact by construction; memcpy/memset call-site liveness clean (4 "reads" were `subfe rD,rA,rA` / `mr rX,rX` idioms); Switch ELF: no new self-loops | PC-verified (leaves), by construction (unguarded); pending console |

### Renderer (host code, ring thread)
| # | Optimisation | Key | Effect | Status |
|---|---|---|---|---|
| 4 | Event-driven WAIT_REG_MEM (Peggle parks the ring there ~29 ms per frame for its frame pacing): woken by interrupts, write pointers and the game's swap hook, at most 1 ms | `masseffect_native_wait_reg_mem_us` (1000; 0 = 50 us polling) | PC: part of the stage-5 figure, 0.59 cores in gameplay (from ~2.4) | pending console |
| 5 | masseffect-nx ring set, default on in Peggle: flat identity, fast pair, raw microcode, PM4 NEON runs, notify_no_lock, compare_fast, fast copy, fingerprint mode 1, dedupe 16384, indices cache, pipelines prewarm (deferred recording: explicit-only since 2026-10-07, see known issues) | `masseffect_*` keys | PC: frames 1000-1399 pixel-identical (frame-aligned recordings); verify mode: 600 k identity-memo, 79 k pairing, 153 k PM4-run, 29 k object-table checks, 0 differences | PC-verified |
| 6 | Relaxed VS identification of D3D-patched fetch swizzles (from futari-nx) | `masseffect_native_vs_relaxed_match` | identifies variants the library lacks; VS identity audit 0 mismatches | PC-verified |
| 7 | Direct renderer step 1 (sa2-nx's): both draw functions (BeginVertices sub_826221A0, DrawIndexedVertices sub_826226A8) hand their state packets to the ring as a host record + one NOP marker. Device fields as SA2's (+48 write pointer, +52/+56 limits, checked in Peggle's disassembly); **BeginVertices leaves +48 alone and keeps its end in device+13844** (EndVertices sub_82622190 copies it), the hook reads it there. Records keep the 3 overwritten words; verify mode re-checks each record against the packets under its marker when the ring applies it | `peggle2_cpu_direct_state` | PC: 100 % of draw calls converted (72,353/72,353), 0 lost; verify 37 k records checked made + applied, 0 diff. Ring time unchanged on the PC (~33 draws/frame: PM4 parsing was never the cost) | PC-verified; console pending |
| 8 | sa2-nx's renderer files of 2026-10-07 (rounds 2-5): EDRAM draw memo (90 % of draws, every hit checked in verify), compare-uploaded, upload buffer cached, lazy trace text, target memo, redirect key, bias seen, entry hash, extent key, estimator reason, rect list in place, identity accept fast + VS mismatch memo (own key here), NoteShaderObject fast, shared hook counter; non-CPU: clear_color0, constants dedupe, pass narrow end (zcull_1x explicit). `NvkSwitchDrawContract` comes with them (finds `nvk_switch_dibujo`) | `peggle2_cpu_ring_*`, `peggle2_cpu_note_shader_fast`, `peggle2_cpu_hook_counter_shared`, `masseffect_native_upload_memory = 1`, `peggle2_native_*` | PC verify (final defaults, 150 s): natives 63.3 M checks + every renderer self-check, 0 differences | PC-verified; console pending |
| 9 | Adaptive vertex dedupe: Peggle never draws the same vertex range twice in a frame (0 hits in every 10 s window, ~19 MB/10 s through XXH3 for nothing); probe 32 frames, rest 256 if no hit | `peggle2_cpu_ring_dedupe_adaptive` | PC: fingerprints 19 -> 2 MB per 10 s; ring CPU change below the PC's noise | PC-verified (exact by construction: a range is copied instead of reused) |
| 11 | Post-codegen passes (tools/perf_codegen_post.py, from futari-nx's): 96,354 direct calls, plain guest memory, 24,826 loop barriers; diet codegen reserved_as_local + skip_msr + dform_disp_split + fctiwz_inline (elide_lr_stores NOT: the heap tracker needs ctx.lr); Switch LTO + -fipa-pta (sa2-nx's set) | `tools/codegen.ps1` (profile `optimised+perf`), `app/perf_overrides.toml`, `PEGGLE2_SWITCH_LTO` | Switch ELF before LTO: recompiled code 5.95 M -> 5.20 M instructions (-12.7 %), hot non-copied functions -5 to -28 % instructions, loads up to -48 %; LTO .text +7 % (inlining); self-loops 3 -> 1 (none new); PC time: within noise (x86) | PC-verified (verify 0 diff over 129.7 M checks; quiet-PC lockstep 122/123); console pending |
| 12 | Smart sync in the localized copies (sa2-nx's), with a runtime key, a verify-mode check at every smart call site (snapshot, call, compare the localized registers outside the callee's reach; out of line) and a planted-bug self-test (`PEGGLE2_LOCALIZE_PLANT=1`: 5 DIFFERENCE lines in 22 s); exact reach for KeTls*, Rtl*CriticalSection, KeBugCheck(Ex); heap-tracker hooks transparent | `peggle2_cpu_natives_smart_sync` | 86 of 295 call sites, 937 register write-back/reload pairs fewer per pass; ~3-4 M checked calls per 10 s, 0 differences | PC-verified |
| 13 | sa2-nx round 7: `fp_moves_modeless` (SA2's SDK change), dedupe sync epochs (correctness, non-CPU key), texture fingerprint sampling OFF (not exact: its guard tripped in 9 of 121 Peggle PC logs) | `fp_moves_modeless` in perf_overrides.toml, `peggle2_native_dedupe_sync_epochs`, `masseffect_native_fingerprints_sampling = 0` | Switch LTO ELF: msr fpcr 13,859 -> 10,495 (-24 % static); verify 133.2 M checks 0 diff; quiet-PC lockstep 122/123 | PC-verified; console pending |
| 10 | Ring timer with the thread's own CPU time (`me_thread_cpu.cpp`: GetThreadTimes / svcGetInfo ThreadTickCount): `[native] ring thread: ... ; CPU C ms per Swap` | - | PC final defaults: ring CPU 0.9-1.5 ms per Swap, 29-43 us per draw (wall "work" includes GPU/present waits) | diagnostic |
| 14 | Codegen: `non_argument_as_local` (sa2-nx's generator + keep list: 180 entries) and `elide_lr_stores` (with Peggle's `lr_keep_callees` for the heap tracker / throw logger) | `perf_overrides.toml` | generated code: ctx.r11 549,939 -> 13,488 uses, ctx.lr stores 148,273 -> 10,681; NRO 54.7 -> 51.8 MB (with row 15) | PC verify 111.9 M checks 0 diff; lockstep 122/123 (fresh seed) |
| 15 | sa2-nx's renderer rounds 6-11 + step 2 + its native Direct3D layer at Peggle's addresses (205 D3D functions paired, identical; address table only): native BeginVertices / DrawVerticesUP / window scissor (`peggle2_api_draws`), direct pair, note-shader pair, identity memo, state refs + restore, bound-plan memo, base-zero r0, native flush (step 2, hooks 8262A4B0/A358/A118), ring zero-ahead, CRC32 fingerprints on the Switch; native Clear explicit-only (known issue) | `peggle2_api_*`, `peggle2_cpu_*` | every BeginVertices/UP draw and scissor write is a native record on the PC route (no fallback); ring PM4 parsing for them gone | PC verify 116.2 M checks 0 diff (+ step 1/2 alone: 28,799 records, 70,210 flushes, 0 diff); lockstep 123/123 at the same game frame |
| 16 | sa2-nx's GPU round + GPU round 2 (new `PEGGLE2_RING_GPU_ENHANCEMENT`, non-CPU master) with the shader package rebuilt by sa2-nx's translator; log_file_writer (SDK) | `peggle2_gpu_*`, `peggle2_native_linear_fetch_not_resolved`, `log_file_writer` | shader package 107,880 -> 141,916 bytes (new variants) | PC verify 144.2 M checks 0 diff; lockstep 121/123 (spinner frames only) |

### Build / platform
| Optimisation | Effect | Status |
|---|---|---|
| Registers as locals: cr/ctr/xer + `non_volatile_as_local` (build pair: `tools/codegen.ps1` vs `-Classic`) | not measured | PC-verified (needed one share_registers mark, see known issues) |
| `-ffp-contract=off -fno-strict-aliasing` on the generated code + app (Switch had them; PC clang added 2026-10-06) | exactness | PC-verified (verify 0 diff after the rebuild) |
| nfsmw NVK (Mesa 26.2.2) linked by full path | renderer's NVK features available | pending console (all earlier NROs had 26.2.1) |
| Asset pack (sa2-nx's PackDevice, with SA2's short-read fix) | `asset_pack`; PC mount < 1 ms, 34 files / 879 MB | PC-verified |

### Known issues (Peggle 2)
- RtlReAllocateHeap's split tail 0x8231CEE4 (entered by conditional branches) needs `share_registers` with `non_volatile_as_local`: without it the PC crashed reading guest 0x60 in the `__finally` funclet.
- First version of the enhancement defaults compared VALUES (`HasNonDefaultValue`): a toml line setting a key to its code default (e.g. `= false`) was overridden back on. Fixed: by cvar source.
- `masseffect_native_deferred_recording`: explicit-only (2026-10-07) after futari-nx's lockstep found it not pixel-identical (shared table). Peggle's own captures cannot settle it: its fades do not reproduce.
- `-fno-math-errno -fno-trapping-math`: OFF here (not settled: captures with them missed a fade, but so did one without them under the same build load; quiet-PC captures are the only ones to trust).
- Peggle 2 is not frame-deterministic even with vblank_per_swap (count 2) + clock_per_swap_hz = 30: the loading spinner, the unicorn idle and the HUD slide-in follow host time (loading lengths; probably its NtSetTimerEx timers). Only ~123 of 400 frames reproduce across two CPU-off baselines; against them the defaults miss only 1-2 spinner frames at the edge where the baselines diverge themselves.
<!-- END PEGGLE 2 -->

---

<!-- BEGIN SONIC TRANSFORMED -->
## Sonic & All-Stars Racing Transformed (sasrt-nx) — D:\sonic transformed\sasrt-nx

Structure: one default.xex (no DLL modules), XDK 21119 (its Direct3D is SA2's code with other addresses). A heavily
threaded game (main, SlRenderer, model-instance, animation, particle, audio threads); on the PC the renderer's ring
thread is the bottleneck on heavy tracks (99.5 % busy at ~2,400 draws a frame on track 2.3, 15-19 fps), the game's
threads are not (main 45 %, SlRenderer 33 %). Every key is in `app/src/sasrt_enhancements.cpp` (table + registered),
masters `sasrt_cpu_enhancements` / `sasrt_noncpu_enhancements`; details in docs/enhancements.md. Profile tools:
`sasrt_pc_profile_s` + `tools/pc_hot_functions.py` / `tools/arm_hot_functions.py` (PC), `switch_profile_stacks` +
`tools/switch_hot_functions.py` (console; with `masseffect_gpu_marks_categories`, on in `sasrt.toml` and every
premade config but verify since 2026-10-06 evening, both outside the master table). Verify: `configs/sasrt_cpu_verify.toml` (`[verify] ... | differences: 0`,
`[hot] ... checks, no difference`). **No console run yet (2026-10-06): everything below is PC-verified, pending
console.**

### Game-specific (guest code)
| # | Optimisation | Key | Hot spot it removes | Verified | Status |
|---|---|---|---|---|---|
| 1 | D3D GPU-wait sleep: `sub_82CD3668` (SA2's `sub_8254F400`, same code) sleeps on the ring's progress | `sasrt_cpu_gpu_wait_sleep` (+ `sasrt_gpu_wait_max_us`) | the spinning thread in Direct3D's GPU waits | PC race: 7.5-9.4 s of every 10 s asleep, 29.6 fps (cap 30); code-hash check | pending console |
| 2 | Native XDK memcpy `sub_82C8EEF0` (byte-identical to SA2's `827F05C0`: same size and FNV hash; SA2's native, forward-overlap → original) | `sasrt_cpu_native_memcpy` | 0.5 % of the busy game threads (PC): 58 -> 1 samples per 10 s in the A/B | PC verify on 3 tracks: over 1 M checks per run, 0 diff (ranges guard) | pending console |
| 3 | Localized copies (`tools/localize_native.py`, loop barriers in every copy): 14 kept, the leaves among the PC profile's hottest functions + `sub_827C56C8` (only save/restore-helper calls) + lower leaves for guard coverage; 22 call-heavy copies dropped after the A/B (slower: see Known issues) | `sasrt_cpu_native_sub_<addr>` each + batch master `sasrt_cpu_natives` (A/B: `configs/sasrt_natives_off.toml`) | the leaves' share of the busy game threads (flat profile: the top function is 1.1 %) | PC verify on 3 tracks: the 10 shadow-guarded leaves 0 diff, every call checked (8 over 1 M checks per run; one false DIFFERENCE from x86's sticky denormal flag, fixed by comparing only the guest FPU-mode bits). PC A/B: within noise for the leaves (x86) | pending console |

### Renderer (host code, ring thread)
| # | Optimisation | Key | Effect | Status |
|---|---|---|---|---|
| 4 | masseffect-nx ring set (fast pair, raw microcode, PM4 NEON runs, flat identity, compare_fast, fast copy, indices cache, notify_no_lock, fingerprint mode 1, dedupe 16384, set-4 differences, NVK emission/cbufs/dynamic/preload, ...) + sa2-nx's constants dedupe | ~45 `masseffect_*`/`sa2_*` keys (table) | PC verify mode: 0 differences over 5.18 M identity, 928 k pairing, 1.31 M PM4-run checks | PC-verified, pending console |
| 5 | Direct3D's null-shader draws after memexport skipped before the EDRAM sync | `sasrt_native_skip_null_draws` | race: EDRAM tiles moved 3.07 M → 2.12 M per 10 s | PC-verified |
| 6 | Shadow map's first aliased clear redirected through the raster-grid 4x view | `sasrt_native_edram4_grid_alias` | with #5: 1.89 M tiles per 10 s; depth imports 4.5-6.0 → 3.2-3.4 ms per Swap (PC) | PC-verified |
| 7 | futari-nx's ring fixes: vertex repacks in cached scratch, no register-file copy per rectangle list, hashed diagnostic keys | always on | (Futari: 91 % of the ring) | PC-verified |
| 8 | Memexport mirrors found by a sorted lookup | always on | 2,900 → 108 live mirrors | PC-verified |
| 9 | Deferred recording (3 keys) | `masseffect_native_deferred_recording` ... | a third SLOWER on this PC | experimental (explicit only) |

### Build / platform
| Optimisation | Effect | Status |
|---|---|---|
| Codegen flavour "optimised": perf_overrides.toml + no-volatile + loop barriers (29,058 headers) + 127,648 thunk call sites made direct (this game's `tools/thunk_calls.py`) + 219,814 direct calls | NRO 80.0 → 69.1 MB; self-loops in recompiled code 1 (classic 4) | pending console. Since 2026-10-06 evening the only NRO (`sasrt.nro`; the user's rule: no classic / optimised A/B pair, compare features by config); the PC tree builds this flavour too, and `tools/check_switch_build.py` refuses an NRO that is classic, has a self-branch outside the game's 4 own halts, or lacks the NVK SDK's Mesa |
| nfsmw NVK (Mesa 26.2.2) linked by full path | — | pending console |
| Handheld GPU 460.8 MHz | `sasrt_handheld_gpu_boost` | pending console |
| `-O0` for `sasrt_register.cpp` (Switch) | GCC -O3 did not finish in 30 min | build time only |
| Shader package harvested from all 20 tracks (guest-memory scan of loaded shader objects), index-only loading | 250 → 1,982 shaders, 0 unidentified on every track; 4 MB in RAM instead of 59 MB | PC-verified |
| Pipeline prewarm from earlier sessions | `masseffect_native_pipelines_prewarm` | pending console |

### Known issues (Transformed)
- Every NRO before 2026-10-06 12:18 shipped the generic Mesa 26.2.1 (`-lvulkan`); the optimised NROs before 12:41 had no loop barriers (removed, never run on a console).
- The renderer ignored the texture exponent bias (FXAA 360: 2x bright edges = the "specks") and rejected draws on a lazy stencil fetch from a 64-bit fixed-point color target (a white race); both fixed (rows in the shared tables).
- The GI volumes' integer number format is ignored (as in the emulated GPU); turning it on gives noisy purple light: open.
<!-- END SONIC TRANSFORMED -->
