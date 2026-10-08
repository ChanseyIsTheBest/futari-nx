# masseffect-nx's optimisations: the ones that matter for every ReXGlue + native-renderer port

Read 2026-10-06 from the local copy `D:\Banjo kazooie port\refs\masseffect-nx` (also `D:\sonic adventure 2\_ref\masseffect-nx`):
`docs/optimization-paths.md` (368 lines, every idea with its console run and setting), `app/futari.toml` (the
shipped best profile), `app/perf_overrides.toml` (codegen options), `tools/direct_calls.py`, `tools/pch_no_volatile.py`,
`tools/pch_no_global_lock.py`, `tools/verify_pch.sh`.

Why it matters more than Unleashed/Marathon: masseffect-nx is a **ReXGlue** port with the **same PM4-ring native
renderer** (`app/src/me`) that futari-nx, banjo-nx, sa2-nx, peggle2-nx and sasrt-nx carry. Every cvar below exists in
our trees already; most codegen options exist in our ReXGlue SDK (`sdk/src/codegen/config.cpp`). Many default OFF in
code and are only ON in masseffect-nx's shipped `futari.toml`.

## 1. Codegen (`perf_overrides.toml`, included by the manifest; codegen + rebuild)

```toml
cr_as_local = true
non_volatile_as_local = true      # r14-r31 as C++ locals: +9 % (t219)
xer_as_local = true
ctr_as_local = true
non_argument_as_local = true      # r0/r2/r11/r12/f0, v32-v63: main thread -2 %; needs share_registers marks
args_in_registers = false         # measured no gain on top of the rest
skip_msr = true                   # with reserved_as_local: -3.4 %
reserved_as_local = true
elide_lr_stores = true            # "diet": main thread 32.4 -> 29.1 ms (-10 %), the best single main-thread result
lr_keep_returns = [ ... ]         #   return addresses that hooks compare must keep their LR store
dform_disp_split = true           #   D-form as base + zero-extended register + displacement
fctiwz_inline = true
[functions]
"0x82AC65E4" = { share_registers = true }   # stack probe + split-off fragments read the caller's registers
```

- `share_registers = true` marks are REQUIRED for functions that read the caller's registers (stack probes, split
  fragments, __finally funclets). Peggle 2 hit this: RtlReAllocateHeap's split tail.
- Post-codegen tools (run after EVERY codegen; `tools/codegen.sh` does):
  - `tools/direct_calls.py`: `sub_X(ctx, base)` -> `__imp__sub_X(ctx, base)` for unhooked functions so GCC can inline
    (t119). Any 82xxxxxx in app/SDK sources or tomls counts as hooked; `--check` fails a tree that lost the step.
  - `tools/pch_no_volatile.py`: guest loads/stores without `volatile` (t153: +15-20 %). Risk: a pure memory-polling loop.
  - `tools/pch_no_global_lock.py`: guest atomics without the global lock (`futari_lockfree_atomics`).
  - `tools/verify_pch.sh`: patches found by search, `--check` so a regeneration cannot silently drop one.
- Also: `-fno-math-errno -fno-trapping-math` on generated code; `-ffp-contract=off -fno-strict-aliasing` REQUIRED
  (GCC otherwise fuses multiply-adds: the executable grew 77.6 -> 78.8 MB when the flag was removed).
- **No gain, don't spend time**: LTO (t122), PGO (t160-t162), `-Os` (worse), hot function ordering, native CRT
  memcpy/wcsstr (noise), native D3D state setters (0.1 us/call; the cost is the flush in the draw body).
  "When the hot functions are already called directly, the compiler has little left to see across files."

## 2. Ring thread (renderer cvars; futari.toml values)

```toml
futari_native_deferred_recording = true      # worker records the vkCmd* calls: ring 62 -> 40 % (t201)
futari_native_deferred_recording_naps = 8
futari_deferred_native_fast = true           # queue published every 16 commands
futari_deferred_native_batch = 16
futari_deferred_native_update = true         # vkUpdateDescriptorSets queued (~70 drains/frame, 1.8-3.8 ms)
futari_native_flat_identity = true           # 0.7-0.9 ms
futari_native_fast_pair = true               # 0.5-0.7 ms
futari_native_raw_microcode = true           # 0.4-0.6 ms
futari_native_pm4_fast = true                # 0.4-0.8 ms
futari_notify_no_lock = true                 # 0.2-0.4 ms
futari_native_compare_fast = true            # 0.5 ms
futari_native_fast_copy = true               # CopyVertices was the top function
futari_native_fast_copy_verify = 256
futari_native_fingerprint_mode = 1           # XXH3 was 2.7 ms
futari_native_dedupe_max_fingerprint = 16384 # XXH3 share of the ring 15 -> 9 %
futari_native_indices_cache = true
futari_native_verify_n = 2048                # self-check window of the switches above
futari_native_output_no_wait = true          # ring spent 20 % in vkWaitForFences
futari_native_slots_work = 3
futari_native_pipelines_prewarm = true       # 1765 pipelines, 16 really compiled
futari_nvk_cache_per_stage = true            # cold pipeline 137 -> 81 ms; NEEDS the patched driver
```

All of them together: ring 28.0 -> 26.2 ms, 25.7 with the fast copy (t252/t253), 0 self-check differences.
`futari_wait_blocking_ring` / `futari_wait_occlusion_us` hook Mass Effect's own D3D GPU wait
(`sub_8222FA98`): each game needs its own address (Peggle 2 did this for `sub_8261D378`).

GPU timestamps: `futari_gpu_marks_categories = false` gives ~50 fewer marks per frame (each a flush + semaphore
release); SA2 ships it false.

## 3. SDK (ReXGlue) settings, masseffect-nx section 4 (cvars already in our SDK)

| cvar | default | what |
|---|---|---|
| `thread_wait_blocking` | true | multi-object waits parked on a wake point instead of 1 ms polls (~1000 wake-ups/s less) |
| `clock_nonblocking` | true | guest clock update never blocks |
| `global_lock_spin` | 48 | bounded spin on the object-table lock (0.2-0.5 ms/frame) |
| `audio_volumemask_sleep_us` | 0 | no sleep in a stub the mixer calls every frame (a Horizon sleep gives away the slice) |
| `kernel_critsec_spin` | 0 | open: critical-section spin before the host waiter (gate with an audio test) |
| `kernel_timestamp_interval_ms` | 1 | open: 4 = 250 wake-ups/s less, but changes the guest-visible tick |

Plus in code: event/semaphore/timer/mutant notify after unlocking (a woken waiter ran into the still-held mutex),
NEON XMA conversion, 2 MB guest memory commit granules (64 KB granules ran out of kernel memory blocks: 2001-0103 at
329 MB), NEON registers saved first in the exception entry.

## 4. Threads

- `futari_exclusive_core` (game thread alone on a core, render thread raised to 0x2B): +1.8 fps in Mass Effect;
  Mass Effect-specific values.
- Rejected: official CPU boost mode (the system throttles the GPU to 76.8 MHz while it is on); core 3 via a custom
  loader descriptor (user decision, stability).
- Native audio DSP kernels with a validate mode (`futari_audio_dsp_native` 0/1/2): 23,411 calls, 0 mismatches.

## 5. Method

Every idea is recorded with its run name and number, including the ones that changed nothing, and is not retried
without new data. Probes (deliberately wrong images: zero-area scissor, one triangle per draw) give upper bounds before
work starts.
