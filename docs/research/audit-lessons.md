# What the UnleashedRecomp-NX and MarathonRecomp-NX performance docs teach (read in full, 2026-10-06)

Sources read end to end: `C:\devkitPro\UnleashedRecomp-NX\docs\SWITCH-PERFORMANCE-AUDIT.md` (1,118 lines),
`...\SWITCH-PERFORMANCE.md` (1,582 lines), `C:\devkitPro\MarathonRecomp-NX\docs\SWITCH-OPTIMISATIONS.md` (601 lines,
171 catalogued optimisations), `...\SWITCH-PERFORMANCE.md` (355 lines). This file lists what the earlier summaries
(`unleashed-nx-cpu-optimisations.md`, `marathon-nx-cpu-optimisations.md`) missed or got wrong, and the rules both
projects work by. Read it before implementing anything from those catalogues.

## 1. The rule both projects hold every change to

"The rendered image and the game's behaviour stay exactly the same": no setting, resolution or shader output change.
For every change the audit writes down WHY it is exact, what could break it, and which key turns it off. Two keys in
Marathon are knowingly not identical and say so (relaxed atomics: PowerPC's own weaker ordering; compact texture heap:
a texture would go black past 16,384 descriptors). Write the same three lines (why exact / risk / key) for every
enhancement you add.

## 2. Measurement method (copy it)

- **Test sets**: one NRO + one `config.toml` per configuration, each configuration differing in ONE group of keys, run at
  the same place. The first log line names the build; one log line per round lists which keys are on.
- **CPU-bound runs**: low resolution, **CPU clocked down to 1020 MHz, GPU at max**, so the game's main thread limits the
  frame. **Compare "game thread ms of work per frame", not FPS**: at the 60 FPS cap savings show only as limiter time.
- **GPU-bound runs**: high resolution, same spot, compare GPU frame ms; per-pass GPU timestamps.
- **Frame-time spread**, not only the average (output is quantised to 16.7 ms steps).
- A CPU sampler (pause thread, `svcGetThreadContext3`, resume, every 1-2 ms), symbolised with the ELF of THAT build;
  attribute waits to callers (Horizon reports a blocked thread with its pc AT the svc, not after it).
- Unleashed's whole sequence: sample -> pick the top functions -> native/codegen change -> verify mode on the console
  (0 mismatches over 2 million calls) -> A/B test set -> next round. Marathon perf8 lesson: choose register-locals
  candidates by COMPILED code (fewer loads/stores), not by profile rank.

## 3. Things that broke and must not be repeated

- **GPU loss in long play** (Unleashed round 10): four renderer changes (dead resolve copies, carried clears, skipped
  depth clears, eager depth transitions) gave `VK_ERROR_DEVICE_LOST` after 15-20 minutes, then a freeze in present with
  NO Atmosphere report (nothing faulted on the CPU). Off by default since; Marathon did not port them. Prime suspect:
  ZCULL (`LOAD_ZCULL` on bad plane data kills the GPU context, per the driver's own comment). Any depth-clear /
  depth-layout optimisation needs 30+ minutes of play before it ships.
- **Guest-kernel changes killed the audio**: the first broad batch (spinning critical sections that skipped the kernel
  signal, lock-free event/semaphore lookup, a plain TLS array) stopped the audio for good at the first in-game cutscene.
  Reverted; then reintroduced one narrow change at a time, each off until it passed "play into a stage until its first
  cutscene; audio keeps going". Gate every sync change with that test.
- **The CPU sampler froze the game**: it held its lock while writing the report to the SD line by line, and the main
  thread took that lock. Reports are now formatted off-lock and written by a background thread in ONE write.
- **PGO and codegen**: a PGO profile only applies to the exact generated code it was collected on. Adding `dcbt`
  prefetches lost the profile of all 130 functions that got one; inlined memcpy lost 1,112 of 1,114; an `-O2` build with
  an `-O3` profile matched 87 %. Collect the profile AFTER the codegen changes, with the same `-O` level.
- **PGO builds need RAM**: Marathon's 1.0.3 PGO compile with `JOBS=12` ran out of memory on a 28 GB machine (another
  build running); `JOBS=6` worked. Unleashed's LTO link took 25-60 minutes. Plan builds accordingly.
- **Marathon perf7 audio gaps**: moving the sound thread off core 0 made it time-slice with busy engine threads
  (guest threads all run at 0x3B, the only time-sliced band). Reverted in perf8.
- **Marathon perf6's Present-without-record-wait never ran** because a flag (`g_needsResize`) was never cleared: verify
  that an enhancement is actually ACTIVE (log counters), not only switched on.
- **Callee-saved/non-volatile locals**: Marathon keeps `non_volatile_as_local` OFF (exception funclets and split
  functions read the caller's r12/r14-r31); Unleashed has it on. Peggle 2 hit exactly this in a ReXGlue port (a split
  tail of RtlReAllocateHeap calling its __finally funclet with the caller's r31; fixed with `share_registers = true`).
- **Wide D-form and plain memory need fault emulation**: a 32-bit wrap lands in a guard region and the fault handler
  emulates it; Marathon's handler decodes every AArch64 load/store form (`switch-check-fault-emulation.py` lists any it
  cannot). In ReXGlue ports check what `guest_memory_switch` / the exception handler can emulate before enabling these.
- **FMA drift**: GCC fuses a separate multiply and add whenever it sees the data flow; codegen passes that show it more
  data flow change roundings. Both projects skip FP functions in their passes unless explicit FMA + `-ffp-contract=off`
  is on, and run an ELF fused-op count check (`switch-fp-check.py` / `switch-fused-check.py`) after every build.
- **Shader gathers not exact on the X1** (Marathon: magenta on texel boundaries) even though exact on paper: verify
  modes on the console catch what paper proofs miss.

## 4. What the earlier summaries missed or got wrong

- Unleashed's renderer-side GPU work is much larger than the CPU work (hand-overs instead of resolve copies: 24 of 26
  copies per frame; alpha-test early-out + sinking; transparent-pixel skip; depth-only pipelines without a fragment
  stage; trimmed vertex/pixel outputs; constants through UBOs read from hardware constant banks; compact 16,384-entry
  texture heap (GPU frame 22.65 -> 21.30 ms); precise barriers; ZCULL "greater"). Most are D3D-level, but the
  constant-bank, compact-heap, depth-only and trimmed-output ideas apply to a PM4-ring renderer too.
- **Audio**: SDL's Switch audio thread runs at 0x3B and can starve and stop forever; both ports feed `audout` from a
  0x20-0x2B pump thread. (ReXGlue's SDK already has a `SwitchAudioSystem` pump on audout: check its priority.)
- **Present on the render thread / without the record wait** (1.06-1.37 ms per frame of the game thread).
- **Resource waits**: game loops that `Sleep(5)` until a resource loads were cut to 0.5 ms polls (frame dips).
- **Mesa disk cache off** whenever a pipeline cache is used (duplicate writes, stale shaders).
- **Vibration dedupe**: one HID IPC per frame saved.
- **2 MB-aligned guest heap commits** (Marathon perf6): fewer TLB misses.
- **Fewer file queries**: size from the open handle (1 IPC instead of ~5); directory listing without a stat per file
  (each stat = 5 IPCs + 3 time-zone conversions).
- **Crash reports**: a lost GPU only leaves the driver's reason if a `VK_EXT_debug_utils` messenger is registered
  (release Mesa gives its messages only to one); `notification type 8` = GPU idle timeout (hang).
- **Overlay FPS** for Status Monitor through SaltyNX (the NRO links its own driver, so nothing else can hook present).
- Unleashed's best single CPU wins were game-specific natives found from the profile (light field up to 20 % of the
  main thread, message dispatch 5 %, map find 3.6 %, material animation 2.8 %); Marathon's were the matrix product +
  box projection (~15 %). Generic CRT/RTTI natives were 0.4-1.4 % each. The profile decides.

## 5. Order that worked for them

1. Profiler + build ID + per-round key list in the log (diagnostics first).
2. Build pipeline: direct calls (+ ELF hook check), LTO; later PGO (re-collected after every codegen change).
3. Codegen: plain memory + loop barriers, register locals (chosen per function), FPSCR tracking / fewer mode switches,
   inline FP compare, constant VMX tables, inline fixed-size memcpy.
4. Natives for the profile's top functions, each with code hash + verify mode + its own key.
5. Guest-kernel sync, one change at a time, each through the audio cutscene test.
6. Renderer: whatever the per-pass GPU profile and the frame log show; long-play test for anything touching depth.
