# MarathonRecomp-NX (Switch): catalogue of CPU optimisations

Research for futari-nx, 2026-10-06. Source tree: `C:/devkitPro/MarathonRecomp-NX`, branch `switch-perf`, HEAD `cc1a851`
("Switch: performance work (perf2-perf9) and the 1.0.3 release"). Their own full catalogue is
`docs/SWITCH-OPTIMISATIONS.md` (601 lines; every key, default, file, status, rejected items); per-round changes are in
`docs/SWITCH-PERFORMANCE.md`, build options in `docs/BUILDING-SWITCH.md`, and per-build profiles in
`dist/switch/MarathonRecomp-perfN/`. Their docs' line numbers are sometimes stale; the ones below are from the code.

## 0. Measured results

All on the console at one CPU-limited spot (Soleanna load-in, CPU 1020 MHz, GPU 1305, memory 2733)
(`docs/SWITCH-OPTIMISATIONS.md:92-108`):

| Build | Result |
|---|---|
| perf3 vs the GitHub NRO | about +20 % CPU and +80 % GPU (the user's estimate) |
| perf4 | 47 FPS; game thread 20.2 ms of work per frame |
| perf5 | 54 FPS (+15 %); 17.3 ms (-14 %) |
| perf6 | 55 FPS; 16.84 ms + 1.37 ms waiting in Present |
| perf7 | 60 FPS (frame cap); 16.06-16.29 ms, 0 ms waiting in Present |

PGO and LTO were never measured on their own (every test build used LTO).

Biggest wins, by measured or profiled share:
1. Native 4x4 matrix product + box projection: ~15 % of the game thread (perf4 profile).
2. Present without the record wait: 1.37 ms per frame.
3. Register locals: ~25 % fewer loads and stores in recompiled code.
4. Plain guest memory, narrow loop barriers, wide D-form (with 3: the perf4 codegen bundle).
5. NEON byte-swapped buffer copies, then deferred buffer unlocks: 5-9 % of the game thread, then ~2.7 % more.
6. Havok MOPP query in a single FPCR mode: 5.1 %.
7. Fewer FPCR mode switches: 3-4 % of the game thread, ~7 % of the workers.
8. Event / critical-section spins + engine threads off core 0: the 2.1 ms per frame of job-event waits.

## 1. Native replacements of hot guest functions

Mechanism (generic XenonRecomp): every `sub_X` is a weak `noinline` alias of `__imp__sub_X` (`PPC_WEAK_FUNC`,
`tools/XenonRecomp/XenonUtils/ppc_context.h:157-160`). The app defines a strong `PPC_FUNC(sub_X){...}` working on
`ctx`/`base` and can fall back to `__imp__sub_X(ctx, base)` (`GUEST_FUNCTION_HOOK` exists too,
`MarathonRecomp/kernel/function.h:351-352`). Mid-asm hooks are only gameplay fixes, none for speed. Every native hook
has a `[Switch]` key read once into a global (`MarathonRecomp/os/switch/perf/native_init.cpp:20-32`). Contract
(`native_hooks.h:9-13`): guest memory above r1, r1 and r3 stay bit-identical to the recompiled code; fall back whenever
that cannot be guaranteed. All C++ with simde/NEON intrinsics (`vld1q_u8_x4`, `vrev32q_u8`, `vqtbl`, `vfmaq_f32`);
no hand-written ARM assembly.

| Guest function | What | Native implementation | Key | Check | Gain |
|---|---|---|---|---|---|
| `sub_82168C48` (+ twin `sub_82272648`) | 4x4 matrix product (VMX128) | `os/switch/perf/native_vector_math.cpp:239-335`, same simde ops in the same order, vectors as locals | `SwitchNativeVectorMath` | `SwitchVerifyNativeVectorMath` runs both and diffs memory, v0-v127, r0-r31, FPSCR (`:171-234`) | with ProjectBox ~15 % |
| `sub_825A1490` | box corners projected | `:341-474`; reciprocal chain once; 8 independent chains, FPCR set once; guest `vrefp/vnmsubfp/vmaddfp` as scalar `__builtin_fmaf` in guest operand order | same | same | 10 % -> 3.5 % |
| `sub_826DE850` | `type_info::operator==` | strcmp + lock-free 1,024-entry memo (`native_rtti.cpp:37-74`) | `SwitchNativeRtti` | — | — |
| `sub_826DEE58` | `__RTtypeid` | from the complete-object locator (`:78-93`) | same | — | — |
| `sub_826DF418` | `__RTDynamicCast` | single inheritance only, 2,048-entry memo (`:110-299`) | `SwitchNativeDynamicCast` | `SwitchVerifyNativeDynamicCast`; identical on 567,480 casts | — |
| `_stricmp`, `strncpy`, `strchr`, 2nd memcpy | CRT strings | 8 bytes at a time (SWAR zero-byte test), no 8-byte read crossing a 4 KB page (`native_crt.cpp:28-40, 295-437`) | `SwitchNativeCrtStrings` | falls back on overlap/wrap | — |
| guest memcpy/memmove/memset/memcpy_s | | `GuestMemmove` (`native_crt.cpp:174-293`): x16-free, 64-byte blocks, aligned destination | always | | correctness (newlib memcpy uses x16) |
| `sub_82546818` / `sub_82546928` | D3D VS/PS float-constant setters | one copy of r6x16 bytes + OR into the dirty word (`native_shader_constants.cpp:21-60`) | `SwitchNativeShaderConstants` | falls back on overlap/wrap | — |
| `sub_825BE758` | archive zlib inflate | host zlib (`native_inflate.cpp:112-199`) | `SwitchNativeInflate` | `SwitchVerifyNativeInflate`: 21,064 entries identical | load time |
| CSD modifier lookup | UI aspect map | 2,048 two-way-set cache (`aspect_ratio_patches.cpp:2285-2341`) | `SwitchModifierCache` | — | 0.3 % |
| `RtlEnter/TryEnterCriticalSection`, `KfAcquireSpinLock`, `KeAcquireSpinLockAtRaisedIrql`, `KeTlsGet/SetValue` | kernel imports | raw `PPC_FUNC(__imp__X)` overrides (`kernel/imports.cpp:2369-2408, 2441-2479, 2503-2511`) | see §6 | | |

## 2. Finding the hot functions

CPU sampler (`os/switch/perf/cpu_profiler_switch.cpp`, from Unleashed): thread at priority 0x1E wakes every 2 ms,
`svcSetThreadActivity(Paused)` + `svcGetThreadContext3` + resume; buckets by (thread, 16-byte code line); in-syscall
samples get LR + 7 stack return addresses; "slow frames" mode keeps samples from frames >= N ms
(`SwitchSlowFrameProfileMs`). Report to stderr every 30 s. `tools/switch-cpu-profile.py` symbolises with `nm` on the
`.debug.elf`; `--hot-list` writes `config/hot_functions.txt`.

perf4 game-thread top (share of running samples): Havok MOPP `sub_828844E8` 5.8 %, `UnlockBuffer<uint32_t>` 5.7 %,
box projection 3.7 %, matrix product 2.1 %, `sub_82638CE8` 1.6 %, `sub_825A22A8` 1.1 %, `sub_826384D8` 1.0 %,
`inflate_fast` 0.8 %, scene walk 0.6 %. Also: FPCR `msr` lines 3-4 % of the game thread (~7 % of workers),
register save/restore helpers ~1.2 %.

perf8 selection by compiled code, not profile rank: each candidate compiled both ways (registers as locals vs in the
context) with `tools/switch-compile-check.py`; kept only if locals give fewer loads/stores and <= 8 more spill slots.
Lists: `config/call_locals.txt` (6,293), `config/fp_no_contract.txt` (49). Finding: 18,211 of 22,964 functions did
MORE loads/stores with locals everywhere (~12.9 % more instructions), hence the selective lists.

## 3. Codegen / recompiler

Register locals via an env override in the patched XenonRecomp (`patches/XenonRecomp-switch-perf.patch:28-42`):
`XENON_RECOMP_REGISTER_LOCALS=1` sets CR/CTR/XER/reserved as locals + skip LR + skip MSR (default since perf6).
`non_volatile_as_local` / `non_argument_as_local` stay OFF (exception funclets and split functions read the caller's
r12, r14-r31). ~25 % fewer loads/stores.

XenonRecomp patch items (generic):
- Plain guest memory (`PPC_GUEST_MEMORY` not volatile) + `PPC_LOOP_BARRIER()` on every backward goto / switch edge and
  before tail calls; MMIO stays volatile (`-DPPC_PLAIN_GUEST_MEMORY`, recompiled code only).
- Narrow loop barrier `asm volatile("" : "+m"(*(uint8_t(*)[0x100000000])base))` (guest memory only, not PPCContext);
  FP/vector functions keep the full barrier.
- Wide D-form (`base + uint64(uint32(r)) + d`): displacement folds into AArch64 addressing; a 4 GB wrap lands in a
  64 KB guard and the fault handler emulates it. 596,884 accesses.
- FPSCR mode tracked across labels (fixed point, <= 8 passes): fewer `msr fpcr`.
- Fewer FPCR mode switches: mode-independent ops (`fmr fabs fneg lfd stfd fcfid fctiwz`, `stfs` via
  `PPCDoubleToFloatBitsAnyMode`, `fcmpu` via branch-free compare) keep the current FZ mode. `lfs/lfsx/frsp` excluded
  (changed FMA counts in 7 functions). 3-4 % of the game thread.
- `dcbt/dcbtst` -> `__builtin_prefetch` (276 sites).
- Relaxed `stwcx./stdcx.`: `__atomic_compare_exchange_n(RELAXED)` + compiler barriers instead of `__sync` (full `dmb`
  per CAS); runtime key `SwitchRelaxedAtomics`.
- `sync/lwsync/eieio` as real `dmb` fences (correctness; libnx threads run on any core).
- Constant VMX tables `alignas(16) constexpr`; `PPC_VECTOR_TABLE` = one NEON `TBL`; `PPC_VECTOR_REVERSE` constant
  shuffle (lvx+stvx folds to plain 16-byte load/store); `vperm` = one `BIC` + one 2-register `TBL` (tested exhaustively).
- Inline branchless `fcmpu` (`__builtin_isless` etc., always_inline).
- `PPC_HOT_FUNC_IMPL` = `__attribute__((hot))` (`.text.hot`).
- Classic codegen switch `XENON_RECOMP_CLASSIC=1` for A/B builds.

Post-pass on the generated C++ (`tools/switch-codegen-pass.py`, all on by default; FP/vector functions left exactly as
written unless listed):
- Leaf locals (5,967 functions): every register a leaf touches becomes a local, written back at return.
- Call locals (~6,197): registers as locals between calls; dirty ones stored before each call and reloaded after;
  save/restore helpers inlined.
- FPSCR mode word as a local.
- Inline fixed-size copies: a call preceded by `li r5,N` -> `__builtin_memmove` (N <= 64) / `__builtin_memset`
  (N <= 256) (527 sites).
- FP integer locals in FP functions (808): 8.8 % fewer loads/stores there.
- FP no-contract list (49): `#pragma GCC optimize("fp-contract=off")` + explicit `vfmaq_f32`, only where the fused-op
  count matched the guest.
- FP single mode for one Havok function (not bit-exact for denormals; 5.1 %).

Direct calls (`tools/switch-direct-calls.py`): `sub_X(ctx, base)` -> `__imp__sub_X(ctx, base)` so GCC/LTO can inline;
any function referenced by a hook keeps the weak alias; `verify-elf` fails the build if a hooked symbol is called
directly. 151,618 calls; also tags 890 hot functions with `PPC_HOT_FUNC_IMPL`.

Not done: byte swapping stays `__builtin_bswap*` (one `rev`); no MMIO path; denormals: VMX with FZ via FPCR, scalar
without; `-frounding-math` kept.

## 4. Compiler / link

`-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fdata-sections`, `--gc-sections`
(no `-mcpu`). `-mtp=soft` makes every `thread_local` a call (motivates the TLS fast paths). Release = `-O3`; O2 options
exist, untested. Recompiled code only: `-fno-strict-aliasing -frounding-math -fno-math-errno -fno-trapping-math
-fomit-frame-pointer -ffixed-x16 -DPPC_PLAIN_GUEST_MEMORY`.

LTO: `-flto -fno-fat-lto-objects`, link `-flto=N -flto-partition=balanced` (used for all test builds; `-ffixed-x16`
also on the link line for lto-wrapper).

PGO (`MarathonRecomp/CMakeLists.txt:524-575`, `os/switch/perf/pgo_switch.cpp`): generate with `-fprofile-generate
-fprofile-update=single -fno-profile-values -fprofile-prefix-path`; `--wrap=fopen` redirects `.gcda` to the SD; a
thread calls `__gcov_dump()`/`__gcov_reset()` every 180 s (HOME kills the process). Use with `-fprofile-use
-fprofile-partial-training -fprofile-correction`; a hash of the `.gcda` set goes on every compile line. 1.0.3 = LTO +
PGO from 257 `.gcda`. No FPS measurement; `switch-fp-check.py` found 543 of 993 FP functions with changed FMA counts.

## 5. Threading and scheduling

Horizon only time-slices priority 0x3B. Guest threads 0x3B; render thread 0x2C (must outrank guest spinners); audio
pump/XMA 0x2B; sampler 0x1E. Placement sets only the preferred core (mask stays all cores): render thread core 1,
pipeline compilers + report writer core 2, engine/job threads round-robin on cores 1-2 off the game thread's core 0
(`SwitchJobWorkersOffMainCore`); sound thread off core 0 was dropped (audio gaps); audio pump/XMA hard-pinned to core 2.

Spins vs sleeps: render queue spins 1,000 not 10,000 (a 0x2C spin is not time-sliced and starves guests); event spin
~512 `yield` on `signaledHint` before sleeping (`SwitchEventSpin`); `SpinUntilFree` 512 x `yield` for critical
sections/spin locks; spin-lock waiters `svcSleepThread(10000)` instead of yield (a Horizon yield only runs
same-priority threads); frame limiter `sleep_until` instead of yield-spinning (`SwitchFrameLimiterSleep`); XMA decoders
wait on an event instead of polling every 2 ms. `KeDelayExecutionThread` unchanged.

## 6. Kernel / runtime fast paths

Critical sections (`imports.cpp:908-1065`; keys `SwitchSleepingGuestLocks`, `SwitchFastCriticalSections`,
`SwitchLeanCriticalSectionLeave`, `SwitchCriticalSectionSpin`): brief spin, then `svcWaitForAddress(owner,
WAIT_IF_EQUAL, 1 ms)`; waiters counted in the unused `LockCount` so the final leave skips `svcSignalToAddress` when
nobody waits; lean leave drops a `dmb`; thread id from the caller's `ctx.r13` instead of a thread_local.

Events/semaphores/dispatcher (mutex + condvar; `std::atomic::wait` unreliable on Horizon newlib):
`SwitchFastEvents` (skip notify with 0 waiters), `SwitchSemaphoreWakeCount` (N `notify_one` not `notify_all`),
`SwitchTargetedDispatcherWakeups` (only objects a `KeWaitForMultipleObjects` waits on bump the global generation);
a lost-wakeup bug fixed.

Guest TLS (`SwitchFastGuestTls`): `KeTlsGetValue` (1,967 call sites) via `mrs tpidrro_el0` + 0x1F8, validated once.

Timers: QPC/QPF/GetTickCount -> `steady_clock`; `mftb` -> `cntpct_el0`. (Check: their `KeQueryPerformanceFrequency`
returns 49,875,000 while `cntpct_el0` counts at 19.2 MHz.)

Allocator: o1heap guest heap behind `RtlAllocateHeap`/`XAllocMem` hooks; `std::bit_ceil`; lazy commit in 16 MB
granules split at 2 MB for 2 MB block mappings (fewer TLB misses).

File I/O: size via `fstat` on the open fd (1 IPC instead of ~5). Logging: macros skip formatting when off.

## 7. Renderer CPU side (D3D-level renderer, no PM4)

NEON byte-swapped copy `CopyByteSwapped` (`vld1q_u8_x4` + `vrev32q_u8`; `UnlockBuffer` was 5-9 %); deferred buffer
unlocks (~2.7 %); Present without the record wait (1.37 ms/frame); Present on the render thread; command batching
(256 per batch, SPSC ring, zero-copy pooled buffers, `ProducerToken`); "is this the D3D thread" = one
`mrs tpidrro_el0` compare; redundant render/sampler state filtering; shader constants kept host-order, sparse copies,
uploads trimmed to registers the shader reads, pixel constants skipped for depth-only, UBO path; pipeline/sampler lookup
caches; persistent pipeline cache keyed by driver SHA-256; plume: redundant bind filtering, persistently mapped
buffers, per-command-list scratch vectors, precise barriers.

## 8. Clocks, memory, other

Only the GPU is boosted in handheld (`apmSetPerformanceConfiguration(0x92220008)`); no CPU clock change. Guest window
4 GB + 64 KB guard, lazy `memalign` + `svcMapProcessCodeMemory` + `svcMapProcessMemory` commits; uncommitted access
faults are emulated byte by byte and resumed through x16 (hence `-ffixed-x16` and an x16-free memmove). Audio via
audout with NEON F32->S16. Vibration IPC deduplicated. Mesa disk cache off.

## 9. Verification tooling

Runtime A/B verifiers (`SwitchVerifyNativeVectorMath`, `...DynamicCast`, `...Inflate`, `...TextureSizes`,
`...ShadowGather`); offline `tools/switch-fp-check.py` (fused vs unfused FP op counts per function: the gate for any
codegen change that could alter FMA contraction), `switch-check-fault-emulation.py`, `switch-direct-calls.py
verify-elf` (every link), `switch-compile-check.py`. PC-side exhaustive/random tests in code comments.

## 10. Generic vs Marathon-specific

Generic: all XenonRecomp patch items, register-locals override, codegen-pass locals/inline copies, direct calls + hook
verification + hot attribute, LTO/PGO/`-fomit-frame-pointer`/`-fno-math-errno -fno-trapping-math`, MSVC CRT natives
(RTTI, dynamic_cast, string functions; addresses per game), all kernel fast paths, Horizon priority/yield rules,
command batching, NEON byte-swapped uploads, 2 MB commits, fewer file queries, frame-limiter sleep, profiler tools.

Marathon-specific: matrix product / box projection natives, D3D setter addresses, zlib inflate, MOPP single mode,
scene-walk prefetch, thread entry addresses, CSD cache, the hot/locals/no-contract lists and PGO profile.

Tried and dropped: `lfs/frsp` in fewer mode switches; call locals everywhere; locals reloading only volatile
registers; native ADX decoder; scalar rsqrt; copying only the locked range; library memcpy for large sizes (x16);
sound thread on core 2; `SwitchShadowGather`.
