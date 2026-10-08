// Localized copies of default.xex's hottest guest functions (tools/localize_native.py, app/src/native/hot_functions.txt;
// headers in app/generated/hot/default). Console profile 2026-10-06: after the game thread stopped spinning, the audio
// mixer thread's routines were the hottest guest code left. Each copy is the recompiled function's own statements with
// its registers in C++ locals, one key per function (a row of futari_enhancements.cpp), a code check of the guest
// bytes it was made from (futari_native.h).
//
// The guard (futari_cpu_verify): the copy runs first in record mode (futari_native_hot_record.h: every guest access
// logs the bytes it touches; its callees are localized too and run in record mode as well), its registers and the
// touched bytes are kept, memory is rolled back, the recompiled original runs from the same registers, and both
// results are compared: every register of the context and every touched byte. The original's result is kept. Counted
// in the "[verify]" line as "hot <address>".

#include "../../generated/default/futari_pch.h"

#include <atomic>
#include <cmath>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "futari_native.h"

#define FUTARI_HOT_KEY(ADDR, WHAT)                                                                                 \
  REXCVAR_DEFINE_BOOL(futari_native_hot_##ADDR, true, "Futari", "Localized native copy of sub_" #ADDR ": " WHAT) \
      .lifecycle(rex::cvar::Lifecycle::kInitOnly);
FUTARI_HOT_KEY(821536C0, "audio mix (VMX)")
FUTARI_HOT_KEY(82127390, "panning / volume vectors")
FUTARI_HOT_KEY(821528D0, "mixer routine")
FUTARI_HOT_KEY(82143450, "mixer routine")
FUTARI_HOT_KEY(8212BE20, "linear-interpolation resampler")
FUTARI_HOT_KEY(821268F8, "mixer routine")
FUTARI_HOT_KEY(82127780, "mixer routine")
FUTARI_HOT_KEY(82129348, "mixer routine")

// Record() of every localized function: defined at the end of this file, in record mode.
#define FUTARI_HOT_RECORD_DECL(ADDR) \
  namespace futari::hot::n_##ADDR {  \
  void Record(PPCContext& ctx, uint8_t* base); \
  }
FUTARI_HOT_RECORD_DECL(821536C0)
FUTARI_HOT_RECORD_DECL(82127390)
FUTARI_HOT_RECORD_DECL(821528D0)
FUTARI_HOT_RECORD_DECL(82143450)
FUTARI_HOT_RECORD_DECL(8212BE20)
FUTARI_HOT_RECORD_DECL(821268F8)
FUTARI_HOT_RECORD_DECL(82127780)
FUTARI_HOT_RECORD_DECL(82129348)
FUTARI_HOT_RECORD_DECL(8207DA20)
FUTARI_HOT_RECORD_DECL(8207AE60)
FUTARI_HOT_RECORD_DECL(8207AF40)
FUTARI_HOT_RECORD_DECL(8215A9F0)
FUTARI_HOT_RECORD_DECL(82140630)
FUTARI_HOT_RECORD_DECL(82140700)

#include "../../generated/hot/default/n_821536C0.h"
#include "../../generated/hot/default/n_82127390.h"
#include "../../generated/hot/default/n_821528D0.h"
#include "../../generated/hot/default/n_82143450.h"
#include "../../generated/hot/default/n_8212BE20.h"
#include "../../generated/hot/default/n_821268F8.h"
#include "../../generated/hot/default/n_82127780.h"
#include "../../generated/hot/default/n_82129348.h"

#include "futari_native_hot_undo.h"

namespace {

using Fn = void (*)(PPCContext&, uint8_t*);

// NaN signs are not part of the comparison (sa2-nx's console finding): with FPCR.DN off a float operation on two NaNs
// returns the first operand's, and GCC may order the operands of a commutative operation differently in two
// compilations of the same statement (the original and its localized copy): -NaN against +NaN. Every NaN of the
// float registers and vector lanes is made the same quiet NaN before comparing; any other bit pattern stays exact.
void CanonicalNaNs(PPCContext& c) {
  PPCRegister* fprs = &c.f0;
  for (int i = 0; i < 32; ++i) {
    if (std::isnan(fprs[i].f64)) fprs[i].u64 = 0x7FF8000000000000ull;
  }
  PPCVRegister* vrs = &c.v0;
  for (int i = 0; i < 128; ++i) {
    for (int lane = 0; lane < 4; ++lane) {
      if (std::isnan(vrs[i].f32[lane])) vrs[i].u32[lane] = 0x7FC00000u;
    }
  }
}

// The same for a touched memory block: equal, or every differing aligned big-endian word is a NaN on both sides.
bool SameMemoryNaNTolerant(const uint8_t* a, const uint8_t* b, uint32_t bytes, bool& nan_only) {
  if (std::memcmp(a, b, bytes) == 0) return true;
  if (bytes % 4 != 0) return false;
  for (uint32_t i = 0; i < bytes; i += 4) {
    if (std::memcmp(a + i, b + i, 4) == 0) continue;
    const auto nan32 = [](const uint8_t* p) {
      const uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu) != 0;
    };
    if (!nan32(a + i) || !nan32(b + i)) return false;
    nan_only = true;
  }
  return true;
}

void Guard(futari::native::Site& site, PPCContext& ctx, uint8_t* base, Fn original, Fn record) {
  auto& undo = futari::hot::g_undo;
  undo.clear();
  futari::hot::g_undo_overflow = false;
  const PPCContext start = ctx;
  record(ctx, base);
  const PPCContext native = ctx;
  thread_local std::vector<uint8_t> after;
  after.clear();
  for (const auto& u : undo) after.insert(after.end(), u.at, u.at + u.bytes);
  // Newest first; a block the copy did not change (a vector load) is left as it is.
  for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
    if (std::memcmp(it->at, it->old, it->bytes) != 0) std::memcpy(it->at, it->old, it->bytes);
  }
  ctx = start;
  original(ctx, base);
  if (futari::hot::g_undo_overflow) {
    return;  // too many accesses to roll back: not checked
  }
  const char* what = "registers";
  size_t offset = 0;
  // fpscr.csr caches the whole host control word, sticky exception flags included on x86 (MXCSR): only the bits the
  // guest controls (rounding, flush) are compared (sasrt-nx's false DIFFERENCE).
  PPCContext original_masked = ctx, native_masked = native;
  original_masked.fpscr.csr &= PPCFPSCRRegister::GuestMask;
  native_masked.fpscr.csr &= PPCFPSCRRegister::GuestMask;
  bool nan_only = std::memcmp(&original_masked, &native_masked, sizeof(PPCContext)) != 0;
  CanonicalNaNs(original_masked);
  CanonicalNaNs(native_masked);
  bool same = std::memcmp(&original_masked, &native_masked, sizeof(PPCContext)) == 0;
  nan_only = nan_only && same;
  if (!same) {
    const auto* a = reinterpret_cast<const uint8_t*>(&original_masked);
    const auto* b = reinterpret_cast<const uint8_t*>(&native_masked);
    while (offset < sizeof(PPCContext) && a[offset] == b[offset]) ++offset;
  } else {
    size_t at = 0;
    for (const auto& u : undo) {
      if (!SameMemoryNaNTolerant(u.at, after.data() + at, u.bytes, nan_only)) {
        same = false;
        what = "memory";
        offset = size_t(u.at - base);
        break;
      }
      at += u.bytes;
    }
  }
  futari::native::ReportVerify(site.what, 1, same ? 0 : 1);
  if (same && nan_only) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
      REXLOG_INFO("[native] {} {:08X}: results equal except for the sign of NaNs (operand order of a commutative "
                  "float operation, compilation-dependent); counted as equal", site.what, site.address);
    }
  }
  if (!same) {
    REXLOG_ERROR("[native] {} {:08X}: verify DIFFERENCE in {} (context offset / guest address {:X}); the localized "
                 "copy is turned off",
                 site.what, site.address, what, offset);
    site.mode.store(futari::native::kOriginal, std::memory_order_relaxed);
  }
}

}  // namespace

#define FUTARI_HOT_HOOK(ADDR, WORDS, HASH)                                                                         \
  REX_HOOK_RAW(sub_##ADDR) {                                                                                       \
    static futari::native::Site site{"futari_native_hot_" #ADDR, "hot " #ADDR, 0x##ADDR, WORDS, HASH};              \
    switch (futari::native::ModeOf(site, base)) {                                                                  \
      case futari::native::kOriginal:                                                                              \
        return __imp__sub_##ADDR(ctx, base);                                                                       \
      case futari::native::kNative:                                                                                \
        return futari::hot::n_##ADDR::Native(ctx, base);                                                           \
      case futari::native::kVerify:                                                                                \
        return Guard(site, ctx, base, __imp__sub_##ADDR, futari::hot::n_##ADDR::Record);                           \
    }                                                                                                              \
  }

REX_EXTERN(__imp__sub_821536C0);
REX_EXTERN(__imp__sub_82127390);
REX_EXTERN(__imp__sub_821528D0);
REX_EXTERN(__imp__sub_82143450);
REX_EXTERN(__imp__sub_8212BE20);
REX_EXTERN(__imp__sub_821268F8);
REX_EXTERN(__imp__sub_82127780);
REX_EXTERN(__imp__sub_82129348);
// Code checks: tools/native_code_hash.py default:<ADDR>:<instructions> (instructions of the generated function).
FUTARI_HOT_HOOK(821536C0, 381, 0xF79C830AF8FCD8AAull)
FUTARI_HOT_HOOK(82127390, 251, 0x28F8F80ACC942989ull)
FUTARI_HOT_HOOK(821528D0, 311, 0x414C6D50133873C9ull)
FUTARI_HOT_HOOK(82143450, 527, 0x6F1637B78CA6DC91ull)
FUTARI_HOT_HOOK(8212BE20, 181, 0x837296C6C08893AAull)
FUTARI_HOT_HOOK(821268F8, 186, 0xDD2B2FCC51C27F2Dull)
FUTARI_HOT_HOOK(82127780, 247, 0x49470DC2CB495EE8ull)
FUTARI_HOT_HOOK(82129348, 257, 0x08346F2EEC836A00ull)

// ---- Record mode: from here on every guest access logs what it touches ----------------------------------------------
#include "futari_native_hot_record.h"
#include "../../generated/hot/default/n_821536C0_record.h"
#include "../../generated/hot/default/n_82127390_record.h"
#include "../../generated/hot/default/n_821528D0_record.h"
#include "../../generated/hot/default/n_82143450_record.h"
#include "../../generated/hot/default/n_8212BE20_record.h"
#include "../../generated/hot/default/n_821268F8_record.h"
#include "../../generated/hot/default/n_82127780_record.h"
#include "../../generated/hot/default/n_82129348_record.h"
#include "../../generated/hot/default/n_8207DA20_record.h"
#include "../../generated/hot/default/n_8207AE60_record.h"
#include "../../generated/hot/default/n_8207AF40_record.h"
#include "../../generated/hot/default/n_8215A9F0_record.h"
#include "../../generated/hot/default/n_82140630_record.h"
#include "../../generated/hot/default/n_82140700_record.h"
