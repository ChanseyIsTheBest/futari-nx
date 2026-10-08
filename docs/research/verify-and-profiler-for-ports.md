# CPU verify config and function-level profiler: how futari-nx does them (for the other ReXGlue ports)

2026-10-06. Both are small and drop into any port that carries the futari-derived native renderer (`app/src/me`)
and the ReXGlue SDK's Switch profiler (`sdk/src/ui/switch_perf.cpp`).

## 1. CPU verify config ("same result" check), IMPORTANT

The user's requirement: a config that runs every CPU enhancement AND proves each one gives the same result as the
original path. The renderer's fast paths already self-check, but only for their first `futari_native_verify_n`
(2048) / `futari_native_fast_copy_verify` (256) uses, and nothing summarised the checks.

futari-nx's changes (copy them):

1. `app/src/me/me_verify.h` (new): `inline std::atomic<uint64_t> g_verify_differences` + `VerifyDifference()`.
2. A `::me::native::VerifyDifference();` line before EVERY `REXLOG_ERROR("[native] ...DIFFERENCE...")` /
   `...MISMATCH...` report: 7 sites in `me_native_system.cpp`, 15 in `futari/futari_native_draws.cpp`, 2 in
   `futari/futari_native_targets.cpp` (all inside braced blocks; a one-shot Python regex did it:
   `^(\s*)(REXLOG_ERROR\("\[native\][^"]*(?:DIFFERENCE|MISMATCH))` -> counter line + the original line).
   Leave out the pipeline-prewarm guard (`C6 prewarm: MISMATCH`): it is a timing heuristic (the prewarm thread's
   guess was useless), it changes no output, and counting it would fail a verify run for nothing (banjo-nx's catch).
   That leaves 23 counted sites.
3. `.range(0, 1000000)` -> `.range(0, 2147483647)` on `futari_native_verify_n` and
   `futari_native_fast_copy_verify` (the cap made "every use" impossible: the registry rejected the value and kept
   2048).
4. A bool cvar `<port>_cpu_verify` (default false). At startup, before the renderer reads them, it sets both counts to
   2147483647 with `rex::cvar::SetFlagByName`.
5. In the renderer's 10-second report (next to "identified by code ... unidentified loads"), when the cvar is on:
   `[verify] checks done: identity memo N, record pairing N, raw microcode N, PM4 runs N, object table N |
   differences: D`. The counts are `verify_n at start - the remaining check_*_ counters`; D is the atomic above. **D must
   be 0**: a difference also logs its DIFFERENCE/MISMATCH line and turns that one fast path off for the session.
6. A premade `<port>_cpu_verify.toml`: every CPU enhancement on + `<port>_cpu_verify = true`. It is slower (every
   comparison runs the original path too), so it is for a test session, not play.

PC result for futari-nx (80 s, scripted Arcade run, all CPU enhancements on): 3,610,189 identity-memo checks, 727,938
pairing, 522,781 raw-microcode, 1,586,240 PM4-run checks, **0 differences**.

Native replacements of game functions (when you add them) need the same: Unleashed's pattern is a per-native verify
key that runs the recompiled function and the native on the same context and compares memory + r3-r10/r13/f1-f13/
v0-v13/FPSCR, sampled 1 call in N (`SwitchVerifyEvery`), counted into the same `[verify]` line.

## 2. Premade configs generated from the enhancement table

`tools/make_enhancement_configs.py` reads the enhancement table (`app/src/futari_enhancements.cpp`, rows
`{"cvar", kCpu|kGpu, "what", "on value", "off value"}`) and writes: the default config, `<port>_cpu_on.toml`,
`<port>_cpu_off.toml` (CPU master off, each key at its off value), `<port>_cpu_verify.toml`, `<port>_profile.toml`.
They differ only in what their name says, so two logs compare directly. One place to add an enhancement.

Futari's table now has 63 entries (45 CPU, 18 non-CPU): masseffect-nx's best-profile ring set (deferred recording,
flat identity, fast pair/raw microcode/PM4, compare_fast, fast_copy, indices_cache, notify_no_lock, fingerprint_mode 1,
dedupe_max_fingerprint 16384), the renderer's existing default-on fast paths, the SDK wait/sync settings, the GPU-side
features and the diagnostics that cost time (diag_reuse, diag_memory_upload, gpu_marks_categories, precise_marks:
"enhancement ON" = diagnostic off). Read/written through `rex::cvar::GetFlagByName` / `SetFlagByName`, which also
reaches the SDK's cvars across the PC DLL boundary.

## 3. Function-level CPU profiler (Switch)

The SDK's sampler (`sdk/src/ui/switch_perf.cpp`, on when `logs/rex/stacks_profile.flag` exists) printed only the top
80 exact pcs per thread as percentages. That splits a hot function over many lines and drops its tail. futari-nx:

1. `switch_perf.cpp`: for active threads print EVERY pc with its raw count:
   ```cpp
   for (size_t k = 0; k < pc_hist.size() && k < (active ? 100000u : 3u); ++k) {
     std::fprintf(f, "   %5.1f%%  pc ", double(pc_hist[k].n) * 100.0 / double(busy));
     PrintAddr(f, pc_hist[k].addr);
     std::fprintf(f, " x%u\n", unsigned(pc_hist[k].n));
   }
   ```
2. A link map from the Switch link: `target_link_options(<exe> PRIVATE "LINKER:-Map=${CMAKE_BINARY_DIR}/<exe>.map")`.
   It says which object each `.text.*` section came from, which matters when several game modules share function names
   (futari's 8 cores all have `sub_88070000`...).
3. `tools/switch_hot_functions.py rex_profile.log --map out/switch/<exe>.map [--top 40] [--thread mainThr]
   [--from-report 3]`: adds the samples up per function (self time) and labels each with its origin (a game core,
   default.xex's recompiled code, the renderer, the app, a library). It prints the top functions over all threads and
   per thread. Only the map parser and `module_of()` (object paths -> labels) are port-specific: adapt the names.
4. The map must come from the SAME build as the NRO that produced the profile.

GPU side: the renderer already reports GPU time per pass category ("GPU per Swap ... other / shadows / scene / copies
...") when `futari_gpu_marks_categories = true`. Leave it false for play (each mark is a flush + semaphore
release) and true in the profile config.

Files to copy: `futari-nx/sdk/src/ui/switch_perf.cpp` (the loop above), `futari-nx/app/CMakeLists.txt` (the Map line),
`futari-nx/tools/switch_hot_functions.py`, `futari-nx/app/src/me/me_verify.h`, `futari-nx/app/src/futari_enhancements.cpp`
(the table + Apply()), `futari-nx/tools/make_enhancement_configs.py`.
