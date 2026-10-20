// Native replacements of hot guest functions: the shared framework (UnleashedRecomp-NX's native_hot_patches /
// native_verify pattern, docs/research/unleashed-nx-cpu-optimisations.md section 4).
//
// A replacement is a strong definition of the recompiled sub_X (REX_HOOK_RAW): it overrides the generated weak alias,
// and the recompiled body stays callable as __imp__sub_X. The body of a replacement is always
//
//   REX_HOOK_RAW(sub_8207DAC0) {
//     static futari::native::Site site{"futari_native_memcpy", "memcpy", 0x8207DAC0, 290, 0x...};
//     switch (futari::native::Mode(site, base)) {
//       case futari::native::kOriginal: return __imp__sub_8207DAC0(ctx, base);
//       case futari::native::kNative:   return NativeMemcpy(ctx, base);
//       case futari::native::kVerify:   return futari::native::Verify(site, ctx, base, {ranges}, __imp__sub_8207DAC0,
//                                                                     NativeMemcpy);
//     }
//   }
//
// Rules for a native (what makes it safe to turn on):
//  - Code check: it runs only if its key is on (and the CPU master, futari_enhancements.cpp) AND the guest code it was
//    written from is what is loaded at its address: FNV-1a of the instruction words. Several game cores share
//    addresses with different code; a wrong core falls back to the recompiled function with one log line.
//  - Result: the same guest memory contents and the same ABI-visible registers as the recompiled function: r1, r2,
//    r13-r31, f14-f31, v14-v31 unchanged unless the original changes them, r3/r4 and f1 (return values) equal. Volatile
//    registers (r0, r4-r12, f0, f2-f13, ctr, xer, cr) are the caller's to lose; with kExact the native must match
//    those too (for functions whose callers are hand-written assembly). Stores below the stack pointer (the red zone)
//    may be skipped.
//  - Verify: with futari_cpu_verify (futari_native_verify_every = N: every Nth call of each native) the recompiled
//    function and the native run from the same state; their memory in the declared ranges and their registers are
//    compared and the ORIGINAL's results are kept. Every check and difference goes into the "[verify]" line.
//
// The header is used by the executable (default.xex replacements) and by each game core (core replacements are
// compiled into the core's own target, since a core's symbols are local to it).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace futari::native {

// Host pointer of a guest address: the generated code's REX_RAW_ADDR (the PC keeps physical memory 0x1000 higher).
inline uint8_t* Host(uint8_t* base, uint32_t address) {
#if defined(_WIN32)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

// A guest range [address, address + size) that one host pointer covers (no crossing into the physical views).
inline bool Contiguous(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && (address >= 0xE0000000u || end <= 0xE0000000u);
}

// FNV-1a 64 over the guest instruction words as stored (big-endian bytes).
inline uint64_t CodeHash(const uint8_t* base, uint32_t address, uint32_t words) {
  uint64_t h = 0xcbf29ce484222325ull;
  const uint8_t* p = Host(const_cast<uint8_t*>(base), address);
  for (uint32_t i = 0; i < words * 4; ++i) {
    h = (h ^ p[i]) * 0x100000001b3ull;
  }
  return h;
}

enum Mode : uint8_t { kOriginal = 0, kNative = 1, kVerify = 2 };
enum class Compare : uint8_t { kAbi, kExact };

struct Site {
  const char* key;    // cvar that turns it on (a row of futari_enhancements.cpp)
  const char* what;   // log name
  uint32_t address;   // guest entry point
  uint32_t words;     // instructions covered by the code check
  uint64_t hash;      // CodeHash of those instructions when the native was written
  Compare compare = Compare::kAbi;
  std::atomic<int8_t> mode{-1};  // -1 = not decided yet
  std::atomic<uint32_t> calls{0};
  uint64_t verify_every = 1;
};

// Totals of every native's verify checks for the "[verify]" line, and the one log line per site saying what it runs.
// The executable keeps them (futari_native.cpp); a game core reaches them through futari_native_core.cpp (the
// executable's export on the PC, the final link on the Switch), since a core's own symbols are local to it.
void ReportVerify(const char* what, uint64_t checks, uint64_t differences);
void ReportDecided(const char* what, uint32_t address, int mode, uint64_t found_hash, uint64_t expected_hash);
// The game's D3D GPU wait sleeps until the ring thread advances (src/me/me_ring_wait.cpp; cores reach it through
// futari_native_core.cpp).
void WaitD3DGpuProgress(uint8_t* base, uint32_t wait);
// Game cores only: the executable's Direct3D census (src/me/futari_d3d_census.h), for the core's hooks (the census
// build's generated ones included).
bool CensusOn();
void CensusEnter(uint32_t address, const uint8_t* base, const char* module);
void CensusLeave(const uint8_t* base);
void CensusSegment(const uint8_t* base, int after);
void CensusNoteDraw(const uint8_t* base, uint32_t device);
// Executable only: "natives: memcpy 1234/0 ..." (checks/differences per native) for the "[verify]" line.
std::string VerifySummary();
// futari_codegen_checks.cpp: the PC side of the post-codegen steps (nothing on the Switch). Init reads the diagnostic
// keys once the config is loaded; Report runs with the renderer's 10 s report.
void CodegenChecksInit();
void CodegenChecksReport(int seconds);
// futari_forms_selfcheck.cpp: the instruction forms only one processor has, against plain C++ references, at every
// start (on the console: the first time the AArch64 forms run). One "[verify]" line; a failure also leaves
// futari_selfcheck_failed.txt next to the program.
void FormsSelfCheck();

inline bool FlagOn(const char* key) {
  const std::string value = rex::cvar::GetFlagByName(key);
  return value == "true" || value == "1";
}

// The first call of a site: key, code check, verify mode (the module's code is loaded then); the result is kept.
// Out of line: only ModeOf's two-compare fast path below sits in every replacement.
[[gnu::noinline]] inline Mode DecideMode(Site& site, uint8_t* base) {
  int8_t mode = site.mode.load(std::memory_order_relaxed);
  if (mode < 0) {
    const uint64_t found = CodeHash(base, site.address, site.words);
    if (!FlagOn(site.key)) {
      mode = kOriginal;
    } else if (found != site.hash) {
      mode = kOriginal;
      ReportDecided(site.what, site.address, -1, found, site.hash);
    } else {
      mode = FlagOn("futari_cpu_verify") ? kVerify : kNative;
      const std::string every = rex::cvar::GetFlagByName("futari_native_verify_every");
      site.verify_every = std::max<uint64_t>(1, std::strtoull(every.c_str(), nullptr, 10));
    }
    site.mode.store(mode, std::memory_order_relaxed);
    if (mode != kOriginal) {
      ReportDecided(site.what, site.address, mode, found, site.hash);
    }
  }
  if (mode == kVerify && (site.calls.fetch_add(1, std::memory_order_relaxed) % site.verify_every) != 0) {
    return kNative;
  }
  return Mode(mode);
}

// What this call of the site runs: one load and a compare once decided (native or original); the first call and
// every call in verify mode go through DecideMode.
inline Mode ModeOf(Site& site, uint8_t* base) {
  const int8_t mode = site.mode.load(std::memory_order_relaxed);
  if (mode == kNative || mode == kOriginal) [[likely]] {
    return Mode(mode);
  }
  return DecideMode(site, base);
}

struct Range {
  uint32_t address;
  uint32_t size;
};

namespace detail {

inline void Gather(uint8_t* base, std::initializer_list<Range> ranges, std::vector<uint8_t>& out) {
  out.clear();
  for (const Range& r : ranges) {
    const uint8_t* p = Host(base, r.address);
    out.insert(out.end(), p, p + r.size);
  }
}

inline void Scatter(uint8_t* base, std::initializer_list<Range> ranges, const std::vector<uint8_t>& in) {
  size_t at = 0;
  for (const Range& r : ranges) {
    std::memcpy(Host(base, r.address), in.data() + at, r.size);
    at += r.size;
  }
}

inline bool SameRegisters(const PPCContext& a, const PPCContext& b, Compare compare, const char*& first) {
#define FUTARI_SAME(reg)                         \
  if (a.reg.u64 != b.reg.u64) {                  \
    first = #reg;                                \
    return false;                                \
  }
  FUTARI_SAME(r1) FUTARI_SAME(r2) FUTARI_SAME(r3) FUTARI_SAME(r13) FUTARI_SAME(r14) FUTARI_SAME(r15)
  FUTARI_SAME(r16) FUTARI_SAME(r17) FUTARI_SAME(r18) FUTARI_SAME(r19) FUTARI_SAME(r20) FUTARI_SAME(r21)
  FUTARI_SAME(r22) FUTARI_SAME(r23) FUTARI_SAME(r24) FUTARI_SAME(r25) FUTARI_SAME(r26) FUTARI_SAME(r27)
  FUTARI_SAME(r28) FUTARI_SAME(r29) FUTARI_SAME(r30) FUTARI_SAME(r31) FUTARI_SAME(f1) FUTARI_SAME(f14)
  FUTARI_SAME(f15) FUTARI_SAME(f16) FUTARI_SAME(f17) FUTARI_SAME(f18) FUTARI_SAME(f19) FUTARI_SAME(f20)
  FUTARI_SAME(f21) FUTARI_SAME(f22) FUTARI_SAME(f23) FUTARI_SAME(f24) FUTARI_SAME(f25) FUTARI_SAME(f26)
  FUTARI_SAME(f27) FUTARI_SAME(f28) FUTARI_SAME(f29) FUTARI_SAME(f30) FUTARI_SAME(f31)
  if (compare == Compare::kExact) {
    FUTARI_SAME(r0) FUTARI_SAME(r4) FUTARI_SAME(r5) FUTARI_SAME(r6) FUTARI_SAME(r7) FUTARI_SAME(r8)
    FUTARI_SAME(r9) FUTARI_SAME(r10) FUTARI_SAME(r11) FUTARI_SAME(r12) FUTARI_SAME(f0) FUTARI_SAME(f2)
    FUTARI_SAME(f3) FUTARI_SAME(f4) FUTARI_SAME(f5) FUTARI_SAME(f6) FUTARI_SAME(f7) FUTARI_SAME(f8)
    FUTARI_SAME(f9) FUTARI_SAME(f10) FUTARI_SAME(f11) FUTARI_SAME(f12) FUTARI_SAME(f13)
  }
#undef FUTARI_SAME
  if (std::memcmp(&a.v14, &b.v14, sizeof(PPCVRegister) * 18) != 0) {
    first = "v14-v31";
    return false;
  }
  return true;
}

}  // namespace detail

// Runs the recompiled function and the native from the same state (registers + the declared memory ranges), compares,
// and leaves the recompiled function's results in place. The ranges must cover every guest byte either one writes.
template <typename Original, typename Native>
void Verify(Site& site, PPCContext& ctx, uint8_t* base, std::initializer_list<Range> ranges, Original&& original,
            Native&& native) {
  thread_local std::vector<uint8_t> before, after_original, after_native;
  detail::Gather(base, ranges, before);
  // PPCContext is aligned to 64 bytes; heap copies would need aligned new: copies live on this stack frame.
  PPCContext start = ctx;
  original(ctx, base);
  PPCContext result = ctx;
  detail::Gather(base, ranges, after_original);
  detail::Scatter(base, ranges, before);
  ctx = start;
  native(ctx, base);
  detail::Gather(base, ranges, after_native);
  const char* first = "memory";
  const bool same = after_native == after_original && detail::SameRegisters(result, ctx, site.compare, first);
  detail::Scatter(base, ranges, after_original);
  ctx = result;
  if (!same) {
    size_t at = 0;
    if (after_native.size() == after_original.size()) {
      while (at < after_native.size() && after_native[at] == after_original[at]) {
        ++at;
      }
    }
    REXLOG_ERROR("[native] {} {:08X}: verify DIFFERENCE (first: {}{}); r3 {:08X} r4 {:08X} r5 {:08X} in", site.what,
                 site.address, first, first[0] == 'm' ? fmt::format(" byte {}", at) : std::string(),
                 start.r3.u32, start.r4.u32, start.r5.u32);
  }
  ReportVerify(site.what, 1, same ? 0 : 1);
}

}  // namespace futari::native
