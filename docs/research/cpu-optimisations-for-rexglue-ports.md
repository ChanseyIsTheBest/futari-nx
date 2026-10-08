# CPU (and non-CPU) optimisations for ReXGlue Switch ports: combined report

From futari-nx, 2026-10-06, for the ReXGlue Switch ports on this machine (Mushihimesama Futari, Sonic Adventure 2,
Banjo-Kazooie: Nuts & Bolts, Peggle 2, Sonic & All-Stars Racing Transformed). Sources, with file:line references and
measured gains:

- `D:\mushihemesama\futari-nx\docs\research\unleashed-nx-cpu-optimisations.md` (UnleashedRecomp-NX,
  `C:\devkitPro\UnleashedRecomp-NX`)
- `D:\mushihemesama\futari-nx\docs\research\marathon-nx-cpu-optimisations.md` (MarathonRecomp-NX,
  `C:\devkitPro\MarathonRecomp-NX`)

Both are XenonRecomp ports with a D3D-level renderer; ours are ReXGlue ports (same lineage: `__imp__sub_X` bodies,
weak `sub_X`, `REX_HOOK_RAW`, a `[rexcrt]` manifest section) with the PM4-ring native renderer, so the codegen items map
to ReXGlue's codegen (`sdk/src/codegen`) and the D3D-renderer items mostly do not apply (see the end).

## The user's requirement: an on/off toggle for every enhancement

Every enhancement, CPU and non-CPU, gets:

1. its own key in the port's toml (cvar), default chosen per enhancement;
2. a group master switch: `<port>_cpu_enhancements = true|false` and `<port>_noncpu_enhancements = true|false`;
   an enhancement is active only when both its own key and its group's master are on;
3. one log line at startup listing every enhancement and whether it is active (so a console log says exactly what ran);
4. for build-time changes that cannot be switched at run time (codegen, compiler flags, LTO/PGO): a build option plus
   an A/B NRO pair (e.g. "classic codegen" vs "optimised codegen"), documented next to the runtime keys.

Unleashed and Marathon already work this way (`[Switch]` keys, verify modes, A/B test sets) and that is how they found
which changes actually helped on the console.

## Measured results to calibrate expectations

- Marathon: 47 -> 60 FPS (frame cap) at a CPU-limited spot, CPU 1020 MHz, over perf4-perf7.
- Unleashed: round 5 -> 6: 48 -> 54 FPS; round 8 CPU group 56.3 -> 59.3; round 11: 52 -> 57.7 at 1020 MHz.
- nfsmw-nx (origin of the direct-calls idea): 30.3 -> ~34 FPS with direct calls + LTO + PGO.

## Priority list (generic first; each item names its source section)

### A. Measure first (both ports did this before every change)
1. Sampling profiler on the console (both: ~2 ms pause + `svcGetThreadContext3` + resume, per-thread CPU via
   `InfoType_ThreadTickCount`, symbolised with `nm`). ReXGlue's SDK already has one (`sdk/src/ui/switch_perf.cpp`,
   enabled by `logs/rex/stacks_profile.flag`); futari-nx adds full per-pc counts and `tools/switch_hot_functions.py`
   (per-function self time from the link map `-Wl,-Map`, labelled by module).
2. Test methodology: CPU-bound spot, CPU fixed at 1020 MHz, A/B with toggles, look at game-thread ms per frame, not
   only FPS (at the frame cap savings show as limiter time).

### B. Build pipeline (largest, generic) — Unleashed §1, Marathon §3-4
3. Direct calls between recompiled functions (`sub_X(ctx, base)` -> `__imp__sub_X(ctx, base)` when X is not hooked)
   + an ELF check that no hook was bypassed. Unleashed `tools\switch-direct-calls.py`. Enables inlining across guest
   functions. In ReXGlue, check how generated calls are spelled and which symbols the app hooks (`REX_HOOK_RAW`,
   `[rexcrt]`, overrides).
4. LTO (`-flto -fno-fat-lto-objects`, link `-flto=N -flto-partition=balanced`). Heavy build RAM.
5. PGO (generate/use flags, `--wrap=fopen` to the SD, `__gcov_dump()` every 180 s). Re-collect after codegen changes.
6. `-O2 -fipa-pta` with PGO (A57 I-cache), `-fno-math-errno -fno-trapping-math`, `-fomit-frame-pointer` for
   recompiled code only, keep `-frounding-math`, `-ffp-contract=off` + explicit FMAs (exactness).
7. Hot section (`__attribute__((hot))`) for the profile's top functions, integer-only ones.

### C. Codegen (generic) — Unleashed §2-3, Marathon §3
8. Registers as locals: CR/CTR/XER/reserved as locals, skip LR/MSR (Unleashed also `non_volatile_as_local`
   + `non_argument_as_local`; Marathon keeps those two off because funclets/split functions read the caller's
   r12/r14-r31; check what your port's exception funclets need). ~25 % fewer loads/stores. Marathon's lesson: locals
   everywhere can be WORSE (18,211 of 22,964 functions did more loads/stores) -> choose per function by compiled
   result (leaf locals, call locals lists).
9. Non-volatile guest memory + loop barriers on backward branches (narrow barrier: guest memory only); MMIO volatile.
10. Wide D-form addressing (`base + uint64(uint32(r)) + d`) + a guard region after the 4 GB window and fault
    emulation for wraps.
11. FPSCR/FPCR: track the flush mode across labels; fewer mode switches with exact any-mode helpers (FPCR writes were
    3-6 % of the game thread in both games).
12. Relaxed `stwcx.`/`stdcx.` (relaxed CAS between compiler barriers) + real fences for `sync`/`lwsync`/`eieio`.
13. `dcbt` -> `__builtin_prefetch`; inline branchless `fcmpu`; constexpr VMX tables -> NEON `TBL`; `vperm` as
    BIC + TBL; scalar rsqrt of dot products.
14. Inline fixed-size memcpy/memset (`li r5,N` before the call -> `__builtin_memcpy/memset`), other calls straight to
    libc.

### D. Native replacements of hot guest functions (generic technique, per-game targets) — Unleashed §4-5, Marathon §1
15. Strong `PPC_FUNC(sub_X)` / ReXGlue `REX_HOOK_RAW(sub_X)` calling `__imp__sub_X` as fallback; each native has its
    own key, a code-hash check of the guest function it was written from (else fall back + log), and a verify mode
    that runs both and compares memory + registers (sampled 1 in N calls). Exactness rule: same guest loads/stores in
    the same order, r3-r10/r13/f1-f13/v0-v13/FPSCR identical, stores below SP may be skipped.
16. Generic-to-every-XDK/MSVC-title targets (addresses differ per game): guest memcpy/memmove/memset/memcpy_s
    (Marathon: x16-free `GuestMemmove` if your fault handler resumes through x16), string functions (8 bytes at a
    time, never crossing a 4 KB page), `type_info::operator==` + `__RTtypeid` + `__RTDynamicCast` with memo tables,
    D3D shader-constant setters (memcpy + OR dirty word), XMemDecompress (LZX) / zlib inflate, message dispatchers
    built from typeid chains.
17. Game-specific targets from YOUR profile (Unleashed: map find, light field 20 %, material animation, Havok
    clusters; Marathon: matrix product + box projection 15 %). "Localized copies" (Unleashed `switch-localize.py`)
    turn a hot cluster's context registers into a stack struct GCC keeps in registers.

### E. Guest kernel / runtime (generic) — Unleashed §7, Marathon §5-6
18. Critical sections on the address arbiter with waiter counting (leave signals only if someone waits), lean leave,
    strong CAS on enter, 512 x `yield` spin first, thread id from `ctx.r13` not TLS.
19. Events/semaphores notify only with waiters, release(1) -> notify_one, targeted dispatcher wakeups.
20. Spin locks: spin then `svcSleepThread(10 us)` (a Horizon yield only runs same-priority threads); Sleep(0) ->
    10 us sleep.
21. Thread priorities: only 0x3B is time-sliced; keep host spinners out of it or bound their spins; render/ring thread
    above guest threads; pin helper threads off the game thread's core.
22. TLS avoidance (`-mtp=soft` makes TLS a call): `mrs tpidrro_el0` for "which thread am I", global lock-free memo
    tables.
23. IO: file size from the open handle, directory listing without a stat per file.
Gate every sync change with an audio-heavy console test: Unleashed's first broad batch killed cutscene audio.

### F. Renderer CPU side
24. For PM4-ring ports (ours): measure the ring thread (in Futari it is ~73 % of a core in gameplay); candidates are the
    ring's per-draw diagnostics/audits, hashing of vertex/constant payloads, redundant state binds, upload copies
    (NEON byte-swap copies, Marathon §7), descriptor updates. Each with its toggle.
25. D3D-level items (command batching, Present without record wait, plume bind filtering) apply to D3D-renderer ports
    only; the ideas (filter redundant binds, persistently mapped buffers, no thread_local scratch) still apply.

### G. Non-CPU
26. Handheld GPU boost (`apmSetPerformanceConfiguration(0x92220008)`, revert if the memory clock moved) — both ports,
    default on.
27. Mesa disk cache off; pipeline cache saved only on misses; vibration IPC dedupe.

## What futari-nx will do

Toggle framework first (master keys + per-enhancement keys + startup summary), then: profile on the console, the
generic CRT natives (`[rexcrt]`/hooks for memcpy/memset/strings found with `tools/d3d_match.py` against Banjo's named
CRT), kernel sync fast paths, the codegen items ReXGlue supports, and natives for Futari's own top functions. Findings
and measured gains will go into this folder.
