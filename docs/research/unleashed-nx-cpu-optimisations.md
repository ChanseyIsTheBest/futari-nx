# UnleashedRecomp-NX (Switch): catalogue of CPU optimisations

Research for futari-nx, 2026-10-06. Source tree: `C:\devkitPro\UnleashedRecomp-NX\` (their docs:
`docs\SWITCH-PERFORMANCE.md`, `docs\SWITCH-PERFORMANCE-AUDIT.md`). Each change has a `[Switch]` key in `config.toml` or
a `SWITCH_*` build option, and is A/B-tested on the console with "test sets" (one NRO + one config per variant).
CPU-bound runs use 858x482 with the CPU at 1020 MHz so the game's main thread limits the frame.

Headline results (`docs\SWITCH-PERFORMANCE.md:38-52`): round 5 -> 6: 48 -> 54 FPS at 480p; round 8 CPU group
56.3 -> 59.3 FPS; round 10 build 53.6 FPS at 1020 MHz; round 11: 52 -> 57.7 FPS; round 12: 58 -> 59 FPS at the hub,
+5 FPS in explosions.

Not in the repo: hand-written `.S` assembly (all C++/NEON + some inline asm), CPU boost / sys-clk, unity builds,
`-fno-plt`, `-mcpu`, a guest allocator replacement, a QPC fast path, a TLS fast path for `KeTlsGetValue`.

## 1. Build pipeline (largest gains; generic)

Flags of the 1.0.0 build (`build\switch-app\build.ninja`):
```
-ffunction-sections -fdata-sections -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
-O2 -flto -fno-fat-lto-objects -fipa-pta -ffp-contract=off -fno-strict-aliasing -frounding-math
-fno-math-errno -fno-trapping-math -fomit-frame-pointer -fprofile-use=.../pgo -fprofile-partial-training
-fno-profile-values -fprofile-correction -Wno-error=coverage-mismatch
LINK: -Wl,--gc-sections -flto=8 -flto-partition=balanced -fipa-pta -ffp-contract=off -fprofile-use ...
```

- **1a Direct calls** (`tools\switch-direct-calls.py`): every function is a weak noinline alias of `__imp__sub_X`, so
  GCC never inlined across guest functions; the pass rewrites `sub_X(ctx, base)` -> `__imp__sub_X(ctx, base)` unless X is
  hooked (any 82/83xxxxxx literal in the app or config counts as a hook); `--verify-elf` fails the build if a hook was
  bypassed. 365,513 calls to 41,315 functions. Credited to nfsmw-nx (30.3 -> ~34 FPS together with LTO+PGO). Gotcha:
  native code must spell helper addresses in relative form (`GUEST_IMPL(82E0, 2C90)`) or their callers lose direct
  calls.
- **1b Hot section**: `PPC_HOT_FUNC_IMPL` = `__attribute__((hot))` -> `.text.hot`, for the profile's top functions
  (`config\hot_functions.txt`, 653 functions; FP-arithmetic functions left out so `hot` cannot change FMA choices).
- **1c LTO** (`CMakeLists.txt:37-49`, `SWITCH_LTO=1`; build RAM heavy, ~25-60 min link). Part of 48 -> 54 FPS.
- **1d PGO** (`UnleashedRecomp\CMakeLists.txt:508-539`; `os\switch\pgo_switch.cpp:37-89`): generate with
  `-fprofile-generate -fprofile-update=single -fno-profile-values` (value profiling reads TLS via `tpidr_el0`, 0 under
  `-mtp=soft`) + `--wrap=fopen` to redirect `.gcda` to `<NRO folder>/pgo/`; a thread `__gcov_dump(); __gcov_reset();`
  every 180 s (HOME kills the process). Use with `-fprofile-partial-training -fprofile-correction
  -Wno-error=coverage-mismatch`, `-fprofile-prefix-path` for stable names. Pitfall: any codegen change invalidates the
  profile per function (prefetch broke 130 profiles, memcpy inlining 1,112 of 1,114) -> collect again after codegen
  changes.
- **1e `-O2` + `-fipa-pta`** (A57's 48 KB I-cache; PGO brings `-O3` back for hot code). Round-10 build 53.6 FPS.
- **1f** `-fno-math-errno -fno-trapping-math` (sqrt = one `fsqrt`), `-frounding-math` kept, `-fomit-frame-pointer`
  for recompiled code only.
- **1g** `-ffp-contract=off` + explicit FMAs (2f).

## 2. Recompiler (XenonRecomp patch, generic)

Upstream config already has `skip_lr skip_msr ctr_as_local xer_as_local reserved_as_local cr_as_local
non_argument_as_local non_volatile_as_local` all TRUE (`UnleashedRecompLib\config\SWA.toml:8-15`).

- **2a** Guest memory not volatile + `PPC_LOOP_BARRIER()` on every backward branch / before switch tables; MMIO stays
  volatile; `SWITCH_CLASSIC_CODEGEN=1` for A/B. Round-8 group 56.3 -> 59.3 FPS.
- **2b** Narrow barrier `__asm__ __volatile__("" : "+m"(*(uint8_t (*)[0x100000000ull])base))` (context registers stay in
  machine registers through loops).
- **2c** Dropped callee-saved prologue stores with `non_volatile_as_local` (73,097 dead stores).
- **2d** FPSCR flush mode tracked across labels (fixpoint, <= 8 passes).
- **2e** Fewer FPCR mode switches (`SWITCH_FEWER_MODE_SWITCHES=1|2`): moves/`fcfid`/`fctidz`/`fctiwz` keep the VMX
  mode; `lfs`/`stfs`/`frsp`/`fcmpu` through exact branchless any-mode helpers (`PPCFloatToDoubleAnyMode`,
  `PPCDoubleToFloatBitsAnyMode`, `compareBits`); mirror case for `vcfsx vcfux vctsxs vrfin vrfiz`. Unconditional
  switches 9,487 -> 4,856, conditional 56,225 -> 40,918 (level 2). 104,882,480 cases tested, 0 differences. FPCR writes
  were 5.3 % of main-thread samples.
- **2f** Explicit FMAs (`fmadd*` -> `__builtin_fma`, `vmaddfp` -> `vfmaq_f32`) with everything else
  `-ffp-contract=off` (GCC had fused 4,134 extra multiply-adds); checked by `tools\switch-fused-check.py`.
- **2g** Scalar rsqrt of dot products (`vrsqrtefp` of a `vmsum3/4fp` -> one scalar `1/sqrtf` splatted; 819 sites).
- **2h** Inline branchless FP compare (`always_inline`, `__builtin_isless`...): 11,669 out-of-line `fcmpu` sites; code
  -1.6 %.
- **2i** `dcbt/dcbtst` -> `__builtin_prefetch` (293 sites).
- **2j** Relaxed `stwcx./stdcx.` (relaxed CAS between compiler barriers instead of `__sync_bool_compare_and_swap` =
  store-release + full `dmb`; 10,695 sites; key `SwitchRelaxedAtomics`); `sync` -> seq_cst fence, `lwsync/eieio` ->
  acq_rel fence.
- **2k** `mftb` -> `mrs cntpct_el0`.

## 3. Post-generation pass (`tools\switch-codegen-pass.py`, generic)

- **Leaf locals** (8,393 leaves): `ctx.rN/fN/vN` -> locals loaded at entry, written back at every return (every
  possibly-written register is written back: MSVC's interprocedural allocation lets callers read volatiles).
- **Wide D-form** `PPC_LOAD_U32_D(rX.u32, d)` = `base + uint64(uint32(r)) + int64(d)` (displacements 0-4095 or from r1);
  a 32-bit wrap lands in a never-committed guard above the 4 GB window (`kernel\memory.cpp:186-193`).
- **Const VMX tables** -> `constexpr`; `lvx/lvlx/lvrx` shuffles -> one `vqtbl1q_u8` (55,274 sites).
- **Narrow barrier** (2b).
- **Inline memcpy** (round 14): a call to the hooked memcpy/memset preceded by `li r5,N` -> `__builtin_memcpy(base+r3,
  base+r4, N)` (940 sites); the other 873 calls go straight to libc, skipping hook marshalling; 183 functions become
  leaves.
- Round-11 group (with runtime natives): 52 -> 57.7 FPS at 1020 MHz.

## 4. Native replacements of hot guest functions

Mechanism: generated `__attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X)`; the app defines a strong
`PPC_FUNC(sub_X)` and calls the original through `__imp__sub_X`:
```cpp
PPC_FUNC(sub_82E26688)
{
    if (!g_nativeLayerMaskTest)               __imp__sub_82E26688(ctx, base);
    else if (g_verify && VerifyThisCall())    Verify("layer mask test", ctx, base, __imp__sub_82E26688, NativeLayerMaskTest, {});
    else                                      NativeLayerMaskTest(ctx, base);
}
```
(`native_hot_patches.cpp:212-221`). Each native runs only if its key is on AND the guest code at its address (and every
helper it folds in) hashes (FNV-1a) to the code it was written from, else the recompiled code runs and stderr says so
(`native_verify.h:23-44`). Exactness rule: same guest loads/stores in the same order; r3-r10, r13, f1-f13, v0-v13 and
FPSCR left exactly as the recompiled code leaves them; stores below SP may be skipped. C++ with NEON/simde; inline asm
only to reproduce GCC's exact FP sequence (quaternion decoder) or to pin values (`__asm__("" : "+w"(x))`) between FPCR
switches / against unwanted fusing.

| Function | Address | Key | Why / gain |
|---|---|---|---|
| `std::map<uint32_t,T>::find` (6 copies) | 82E18B28 ... | SwitchNativeMapFind | one copy 3.6 % of game thread |
| Render-layer mask test | 82E26688 | SwitchNativeLayerMaskTest | 8x64-bit AND without per-word bswap |
| Quaternion decode (5-byte packed) | 82FCFA08 | SwitchNativeQuatDecode | NEON + asm |
| Bone palette upload into VS constants | 82E18118 | SwitchNativeBonePalette | — |
| Resource-wait loops Sleep(5) -> 0.5 ms | 5 sites | SwitchFastResourceWaits | load dips |
| Visibility test + prefetch two ahead | 82E26498 | SwitchNativeVisibilityTest, SwitchRenderWalkPrefetch | half its samples were one miss |
| CRI handle search (masks not divisions) | 83167518 | SwitchNativeCriHandleSearch | — |
| Shared-string "less than" | 82DFAF58 | SwitchNativeNameCompare | — |
| Light-field sample (FPCR writes ~30 -> 3) | 82E2B780 | SwitchNativeLightField | up to 20 % of main thread |
| `type_info::operator==` + lock-free memo; `__RTtypeid` | 831B0AB8, 831B2438 | SwitchNativeRtti | 0.7 % |
| D3D VS/PS constant setters (memcpy + OR dirty word) | 82BDFAA0, 82BDFB80 | SwitchNativeShaderConstants | 1.4 % |
| XMemDecompress (XDK LZX) | 831CE0D0 | SwitchNativeDecompress, SwitchFastNativeDecompress | 42 % of loader thread |
| 227 message dispatchers (typeid chains -> one hash lookup) | generated list | SwitchNativeMessageDispatch | 5 % of game thread |
| CRI ADX decoder | 8316AFE0 | SwitchNativeAdxDecoder | 27.7 % of sound server |
| CRI reverb/mix/IIR/resampler (localized copies) | 6 sites | SwitchNativeReverb ... | mixer lock 0.44 ms/frame |
| Small-object pool pop/push/free | 82E02BD8 ... | SwitchNativePoolAllocator | 2.3 % |
| Material UV keyframe search + lerp | 82E46FA0 | SwitchNativeMaterialAnimation | 2.8 % (hottest at hub) |
| Havok spline sampler (+6 helpers) | 82FC4390 | SwitchNativeSplineAnimation | 3.5 % |
| Path-following projection (+12 helpers) | 822D22C8 | SwitchNativePathFollowing | 3.7 % |
| Havok MOPP VMs | 82F78148, 82F78FB0 | SwitchNativeMoppVm | 1.9 % |
| Scene-graph walk prefetch (mid-asm hooks) | 82E1B950, 82E1AB70 | SwitchSceneGraphPrefetch | 39 % / 63 % of loop samples were misses |
| UI modifier lookup caches | — | SwitchModifierCache ... | 0.3-0.4 % |

Verification: verify mode (`native_verify.h:55-111`; `SwitchVerifyNativeHotFunctions`, `...Audio`,
`...MessageDispatch`): snapshot memory, run recompiled, restore, run native on a context copy, compare memory + r3..v13,
keep the recompiled results; sampled 1 call in `SwitchVerifyEvery`; >2 million calls, 0 mismatches. Localized copies
are included twice (second time with logged stores and a `longjmp` on leaving calls). PC harnesses: ADX 900,000 random
calls, resampler 30 M samples, material 300,000 calls, any-mode helpers 105 M cases.

## 5. Localized copies (`tools\switch-localize.py`, generic tool)

Copies chosen recompiled functions with `ctx.rN` -> `regs.rN` (a `LocalRegs` struct whose address never escapes, so
GCC keeps it in registers and loop barriers don't spill it); helpers inlined (`always_inline`); FPSCR cached mode local;
calls out store/reload the context: `{ LOCAL_BEFORE_CALL(); regs.Store(ctx); sub_X(ctx, base); regs.Load(ctx); }`.
Loops can be swapped for hand-written code only if the guest mnemonics match exactly. Output outside the app folder so
the direct-calls pass doesn't treat the addresses as hooks.

## 6. Finding hot functions

Sampler `os\switch\cpu_profiler_switch.cpp`: priority 0x1E, every 2 ms pause + `svcGetThreadContext3` + resume;
16-byte code-line buckets; per-thread CPU via `InfoType_ThreadTickCount`; syscall waits attributed via LR + 7 stack
returns; slow-frame mode; top 150 lines per thread every 30 s in one off-lock write (a lock held during the write caused
freezes). Symbolizer `tools\switch-cpu-profile.py` (`--hot-list`). Non-function hot spots: FPCR writes 5.3 %, CS syscall
line 10 %, `__aarch64_read_tp` 0.7 %, `pthread_self` 0.3 %; main thread ~13 % in syscalls.

## 7. Guest-kernel synchronisation (generic, `kernel\imports.cpp`)

| Change | Key | Gain |
|---|---|---|
| Critical sections on the address arbiter (`svcWaitForAddress`, 1 ms safety timeout) | base | — |
| Fast CS leave: waiters counted in `LockCount`, signal only if waiting | SwitchFastCriticalSections | with fast events 51 -> 53.3 FPS, -1.2 ms |
| Lean leave (no full fence between STLR and LDAR) | SwitchLeanCriticalSectionLeave | — |
| Strong CAS on enter (weak CAS failures caused up to 1 ms waits) | SwitchStrongCriticalSectionCas | — |
| 512 x `yield` (~2 us) spin before the kernel wait | SwitchCriticalSectionSpin | — |
| CS thread id from the caller's `ctx.r13` | always | — |
| Events/semaphores notify only with waiters | SwitchFastEvents | see above |
| Targeted dispatcher wakeups | SwitchTargetedDispatcherWakeups | CRI server spent 2/3 of samples in spurious wakeups |
| Semaphore release(1) -> notify_one | SwitchSemaphoreWakeOne | — |
| Guest spin locks: 512 iterations then 10 us `svcSleepThread` | SwitchGuestSpinBeforeSleep | — |
| Sleep(0) -> 10 us sleep (Horizon yield can't run lower priorities) | SwitchFastResourceWaits | dip fixes |

The first, broader batch (spinning CS, lock-free events, plain TLS array) killed the audio at the first cutscene and was
reverted: each change was then gated by that audio test.

## 8. Threads

Render thread 0x2D, main/present 0x2C (libnx starts pthreads at 0x3B, the only time-sliced band); moodycamel spin
10,000 -> 1,000 (a 0x2D spin is not time-sliced); render thread core 1, pipeline compilers core 2
(`SwitchHostThreadCores`, 59.0 vs 59.3 FPS off); `SetThreadIdealProcessor` n -> core n/2 (off, A/B only);
`KeSetBasePriorityThread` mapped to 0x1C-0x30; audio pump 0x20 feeding audout; frame limiter `sleep_until`; Present on
the render thread (52 -> 53.6 FPS), Present without the record wait, per-resource lock waits.

## 9. Renderer CPU side (D3D-level)

Command batching (bulk enqueue per draw, batches of several draws, 512 while idle, producer token, zero-copy pooled
buffers); redundant render/sampler state filtering; shader constants host-order, swap+compare+store in one pass, sparse
copies, uploads trimmed to read registers, no pixel constants for depth-only; pipeline/sampler lookup caches;
statistics with plain loads/stores; plume: redundant bind filtering, persistently mapped buffers, per-command-list
scratch instead of thread_local, pipeline cache saved on misses, lazy push constants; vibration dedupe.

## 10. TLS avoidance (`-mtp=soft` makes TLS a call)

D3D-thread test via `mrs tpidrro_el0` (`__aarch64_read_tp` was 0.7 %); CS r13 from the caller's context; RTTI memo
global lock-free; caches keyed outside TLS; plume scratch vectors.

## 11. Loading / IO

Native LZX; directory listings from `dirnext` entries instead of a stat per file (each stat = 5 IPCs + 3 time-zone
conversions); file size from the open handle; pipeline cache saved only after misses.

## 12. Clocks

Only a handheld GPU boost (`apm 0x92220008`: GPU 460.8, memory 1331.2; reverted if memory moved). No CPU boost; the
1020 MHz CPU is test methodology only.

## 13. Tried / not done

Reverted: the first guest-kernel batch (audio). Not done: caching `KeSetBasePriorityThread`, priorities on 0x3B,
native particle walks, fusing `lvlx/lvrx/vor`, vectorising the CRI mixer, exact helpers for scalar arithmetic inside
vector code (FPCR switches still 5-6 %), batched streaming uploads. `SwitchNvkFastPaths` off (no measured gain).

## 14. Generic vs Unleashed-specific

Generic: the whole build pipeline (direct calls + ELF hook check, hot section, LTO/PGO with the gcov fopen wrap and
periodic dump, `-O2 -fipa-pta`, math/frame-pointer flags), all recompiler items, the codegen pass, the localize tool and
native-hook framework (code hash, verify-by-replay, sampled verify), the guest-kernel sync changes, TLS avoidance,
thread priorities/cores, the sampler + symbolizer. Generic with per-game addresses: RTTI natives, XMemDecompress LZX,
D3D constant setters, message-dispatch-by-type-table, file-system IPC reductions. Unleashed-specific: the individual
hot-function natives, resource-wait loops, scene-graph prefetch hooks, UI caches, hot/dispatch lists.
