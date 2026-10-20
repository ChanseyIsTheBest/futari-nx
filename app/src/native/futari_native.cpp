// Native replacements: the executable's side of the framework (futari_native.h): the verify totals for the "[verify]"
// line, the per-site decision log, and the exports through which the game cores report theirs.

#include "futari_native.h"

#include <atomic>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/logging.h>

#include "me/me_verify.h"

REXCVAR_DEFINE_INT32(futari_native_verify_every, 1, "Futari",
                     "With futari_cpu_verify: compare each native replacement with the recompiled function on every "
                     "Nth call (1 = every call)")
    .range(1, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// One key per native (a row of futari_enhancements.cpp each). The game cores read them by name.
REXCVAR_DEFINE_BOOL(futari_native_lzss, true, "Futari",
                    "Native LZSS decompression in the game cores (the streamed data of gameMainThr), checked against "
                    "the guest code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_memset, true, "Futari",
                    "Native XDK memset in default.xex and the game cores (host memset; sa2-nx's n_827EFEA0, the same "
                    "bytes in all nine modules)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_native_memcpy, true, "Futari",
                    "Native XDK memcpy in default.xex and the game cores (host memcpy; overlapping copies keep the "
                    "recompiled code)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace futari::native {
namespace {

struct Totals {
  std::string what;
  uint64_t checks = 0;
  uint64_t differences = 0;
};

std::mutex g_mutex;
std::vector<Totals> g_totals;

}  // namespace

void ReportVerify(const char* what, uint64_t checks, uint64_t differences) {
  if (differences) {
    ::me::native::g_verify_differences.fetch_add(differences, std::memory_order_relaxed);
  }
  std::lock_guard lock(g_mutex);
  for (Totals& t : g_totals) {
    if (t.what == what) {
      t.checks += checks;
      t.differences += differences;
      return;
    }
  }
  g_totals.push_back({what, checks, differences});
}

void ReportDecided(const char* what, uint32_t address, int mode, uint64_t found_hash, uint64_t expected_hash) {
  if (mode < 0) {
    REXLOG_WARN("[native] {} at {:08X}: guest code differs (hash {:016X}, written for {:016X}): recompiled code runs",
                what, address, found_hash, expected_hash);
  } else {
    REXLOG_INFO("[native] {} at {:08X}: {}", what, address, mode == kVerify ? "native, verified against the original"
                                                                            : "native");
  }
}

}  // namespace futari::native

#if !defined(__SWITCH__)
// tools/flush_mode_flow.py (sasrt-nx, through sa2-nx): off the Switch every flush-mode check the control flow proved
// redundant keeps its test (REX_FLUSH_KNOWN_ON / _OFF in the generated pch), counts itself here and calls
// RexFlushFlowMismatch when the proof was wrong, before the mode is switched after all. So every PC run checks the
// proofs it executes; the Switch build has no code at these places. (A game core has its own counter and reports a
// failed proof through futari_native_core.cpp.)
extern "C" {
volatile unsigned long long g_rex_flush_flow_checked = 0;  // a plain lossy count: "at least this many"
void RexFlushFlowMismatch(int on) noexcept {
  static std::atomic<uint32_t> logged{0};
  futari::native::ReportVerify("flush-mode flow", 0, 1);
  if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[verify] DIFFERENCE: flush-mode flow: a check proven redundant had to switch the mode {} (return "
                 "address {})", on ? "on" : "off", static_cast<const void*>(__builtin_return_address(0)));
  }
}
}
#endif

namespace futari::native {

std::string VerifySummary() {
#if !defined(__SWITCH__)
  {
    static unsigned long long reported = 0;
    const unsigned long long now = g_rex_flush_flow_checked;
    if (now > reported) {
      ReportVerify("flush-mode flow", now - reported, 0);
      reported = now;
    }
  }
#endif
  std::lock_guard lock(g_mutex);
  if (g_totals.empty()) {
    return "natives: none called";
  }
  std::string out = "natives:";
  for (const Totals& t : g_totals) {
    out += fmt::format(" {} {}/{}", t.what, t.checks, t.differences);
  }
  return out + " (checks/differences)";
}

}  // namespace futari::native

// For the game cores (futari_native_core.cpp).
#if defined(_WIN32)
#define FUTARI_NATIVE_EXPORT extern "C" __declspec(dllexport)
#else
#define FUTARI_NATIVE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

FUTARI_NATIVE_EXPORT void FutariNativeVerifyReport(const char* what, uint64_t checks, uint64_t differences) {
  futari::native::ReportVerify(what, checks, differences);
}

FUTARI_NATIVE_EXPORT void FutariNativeDecided(const char* what, uint32_t address, int mode, uint64_t found_hash,
                                              uint64_t expected_hash) {
  futari::native::ReportDecided(what, address, mode, found_hash, expected_hash);
}
