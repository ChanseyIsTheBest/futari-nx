// The PC side of the post-codegen steps of the CPU rounds of 1.0.1 (sa2-nx's sa2_codegen_checks.cpp, for nine
// modules; docs/futari-notes.md, "The CPU rounds of 1.0.1"). The Switch build has no code at any of these places: the
// macros of the generated pch are the bare operation there. Off the Switch each keeps the old operation next to the
// new one:
//   - tools/ro_constants.py: a load from read-only data folded to its constant still loads, counts itself
//     (g_rex_ro_checked) and calls RexRoMismatch when the memory does not hold the constant;
//   - tools/indirect_calls.py: a promoted indirect call counts itself (g_rex_promoted_calls) and calls
//     RexPromotedMismatch when the dispatch table's entry for the guarded target is not the function called;
//   - the census of indirect calls (futari_diag_indirect_census = a file): per module and call site how often it ran
//     and where to, the input of tools/indirect_promotions.py;
//   - the SDK's kernel_critsec_pcr_owner: every critical section call compares the owner id it took from the PCR with
//     the looked-up thread's (rex/sys_counters.h, kCensusCritSecOwnerMismatch);
//   - futari_diag_rdata_check = a folder with the generator's image dumps: the read-only data of every loaded module
//     is compared with its dump, import records aside (the loader writes those). Folding assumes exactly that, also
//     for the loads no PC session reaches.
// banjo-nx's codegen round (2026-10-19; docs/futari-notes.md, "Banjo's codegen round"):
//   - tools/r1_local.py: after every call the r1 mirror keeps (proven, or assumed for indirect calls, hooks and
//     imports) the PC build compares it with ctx.r1 (REX_R1_CHECK: g_rex_r1_checked, RexR1Mismatch, which also
//     reloads the mirror);
//   - tools/lr_preserve.py: every epilogue write-back proven to store the value ctx.lr holds compares the two
//     (REX_LR_WRITE_SAME_CHECK: g_rex_lr_write_checked, RexLrWriteMismatch) and writes as before;
//   - tools/ro_constants_cfg.py: its folds load and compare like tools/ro_constants.py's (RexRoMismatch);
//   - tools/fp_sign_ops.py is exact while the game never selects round up or round down: the "[census] FPSCR
//     rounding mode" line counts the guest's FPSCR writes by mode (rex/sys_counters.h, kCensusRound*).
// So every PC run tests what it executes; the "[verify]" line carries the counts. A game core is a DLL on the PC: it
// has its own counters (futari_native_core.cpp) and reports through the exports at the end of this file.
#include "futari_native.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/sys_counters.h>

REXCVAR_DEFINE_STRING(futari_diag_indirect_census, "", "Futari",
                      "Diagnostic (PC): a file that gets, with every 10 s report, the census of the game's indirect "
                      "calls (per module and call site: calls and targets). tools/indirect_promotions.py makes "
                      "app/indirect_promotions/<module>.txt from it. Empty = no census")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(futari_diag_rdata_check, "", "Futari",
                      "Diagnostic (PC): the folder of the code generator's image dumps (out). With every 10 s report "
                      "the read-only data of default.xex and of the loaded game core is compared with its dump "
                      "(import records aside). Empty = no comparison")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if !REX_PLATFORM_SWITCH

#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>

extern "C" {
volatile unsigned long long g_rex_ro_checked = 0;  // plain lossy counts: "at least this many"
volatile unsigned long long g_rex_promoted_calls = 0;
volatile unsigned long long g_rex_r1_checked = 0;
volatile unsigned long long g_rex_lr_write_checked = 0;
unsigned char g_rex_indirect_census = 0;
}

namespace {

std::atomic<uint64_t> g_ro_mismatches{0};
std::atomic<uint64_t> g_promoted_mismatches{0};
std::atomic<uint64_t> g_r1_mismatches{0};
std::atomic<uint64_t> g_lr_mismatches{0};

void R1Mismatch(const char* module, unsigned long long expected, unsigned long long got) {
  futari::native::ReportVerify("r1 mirror", 0, 1);
  if (g_r1_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[verify] DIFFERENCE: r1 mirror ({}): after a call the mirror held {:016X}, ctx.r1 {:016X} (a callee "
                 "assumed to return r1 unchanged did not; the mirror is reloaded)", module, expected, got);
  }
}

void LrWriteMismatch(const char* module, unsigned long long held, unsigned long long written) {
  futari::native::ReportVerify("link-register write-backs", 0, 1);
  if (g_lr_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[verify] DIFFERENCE: link-register write-back ({}): ctx.lr held {:016X}, the epilogue writes {:016X} "
                 "(a site proven to write the value ctx.lr holds)", module, held, written);
  }
}

// One call site: module, function address, ordinal in the function. Up to four targets are told apart; the rest is
// "other".
struct Site {
  uint64_t total = 0;
  uint32_t target[4] = {};
  uint64_t count[4] = {};
  uint64_t other = 0;
};
struct Key {
  const char* module;  // a string literal of the reporting module: compared by content
  uint32_t function;
  uint32_t site;
  bool operator==(const Key& o) const {
    return function == o.function && site == o.site && std::strcmp(module, o.module) == 0;
  }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = (uint64_t(k.function) << 32 | k.site) * 0x9E3779B97F4A7C15ull;
    for (const char* c = k.module; *c; ++c) h = (h ^ uint8_t(*c)) * 0x100000001B3ull;
    return size_t(h);
  }
};
constexpr size_t kStripes = 64;
struct Stripe {
  std::mutex mutex;
  std::unordered_map<Key, Site, KeyHash> sites;
};
Stripe g_stripes[kStripes];

void Census(const char* module, unsigned function, unsigned site, unsigned target) {
  const Key key{module, function, site};
  Stripe& stripe = g_stripes[KeyHash()(key) >> 58];
  std::lock_guard<std::mutex> lock(stripe.mutex);
  Site& s = stripe.sites[key];
  ++s.total;
  for (int i = 0; i < 4; ++i) {
    if (s.target[i] == target || s.count[i] == 0) {
      s.target[i] = target;
      ++s.count[i];
      return;
    }
  }
  ++s.other;
}

void RoMismatch(const char* module, unsigned address) {
  futari::native::ReportVerify("read-only constants", 0, 1);
  if (g_ro_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[verify] DIFFERENCE: read-only constants ({}): guest memory at {:08X} does not hold the constant "
                 "the generated code uses", module, address);
  }
}

void PromotedMismatch(const char* module, unsigned target) {
  futari::native::ReportVerify("promoted indirect calls", 0, 1);
  if (g_promoted_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[verify] DIFFERENCE: promoted indirect call ({}): the dispatch table's entry for {:08X} is not the "
                 "function the generated code calls", module, target);
  }
}

void WriteCensus(int seconds) {
  struct Row {
    Key key;
    Site site;
  };
  std::vector<Row> rows;
  for (Stripe& stripe : g_stripes) {
    std::lock_guard<std::mutex> lock(stripe.mutex);
    for (const auto& [key, site] : stripe.sites) rows.push_back({key, site});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.site.total != b.site.total) return a.site.total > b.site.total;
    if (int c = std::strcmp(a.key.module, b.key.module)) return c < 0;
    return a.key.function != b.key.function ? a.key.function < b.key.function : a.key.site < b.key.site;
  });
  uint64_t calls = 0, single = 0, single_calls = 0, top_calls = 0;
  for (const Row& r : rows) {
    calls += r.site.total;
    const uint64_t top = *std::max_element(r.site.count, r.site.count + 4);
    top_calls += top;
    if (top == r.site.total) {
      ++single;
      single_calls += r.site.total;
    }
  }
  static uint64_t last_calls = 0;
  REXLOG_INFO("[census] indirect calls: {:.0f} per second; {} sites seen so far with {} calls, {} sites ({} calls) "
              "always went to one target, {:.1f} % of all calls went to their site's most frequent target",
              double(calls - last_calls) / double(std::max(seconds, 1)), rows.size(), calls, single, single_calls,
              calls ? 100.0 * double(top_calls) / double(calls) : 0.0);
  last_calls = calls;
  const std::string path = REXCVAR_GET(futari_diag_indirect_census);
  const std::string temp = path + ".tmp";
  if (FILE* f = std::fopen(temp.c_str(), "w")) {
    std::fprintf(f, "# futari-nx indirect call census (cumulative): <module> <function> <site> <calls> "
                    "<target>:<calls>... other:<calls>\n");
    for (const Row& r : rows) {
      std::fprintf(f, "%s %08X %u %llu", r.key.module, r.key.function, r.key.site,
                   static_cast<unsigned long long>(r.site.total));
      for (int i = 0; i < 4; ++i) {
        if (r.site.count[i]) {
          std::fprintf(f, " %08X:%llu", r.site.target[i], static_cast<unsigned long long>(r.site.count[i]));
        }
      }
      std::fprintf(f, " other:%llu\n", static_cast<unsigned long long>(r.site.other));
    }
    std::fclose(f);
    std::remove(path.c_str());
    std::rename(temp.c_str(), path.c_str());
  } else {
    REXLOG_WARN("[census] indirect calls: cannot write {}", temp);
  }
}

// futari_diag_rdata_check: the .rdata section of one loaded module against the generator's dump of its image.
uint32_t Le32(const std::vector<uint8_t>& d, size_t at) {
  return at + 4 <= d.size() ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 |
                                  uint32_t(d[at + 3]) << 24
                            : 0;
}

void CheckReadOnlyData(rex::system::UserModule* module, const std::string& folder) {
  if (!module || !module->xex_module()) return;
  std::string stem = module->name();
  if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
  const std::string path = folder + "/" + stem + "_image.bin";
  std::vector<uint8_t> dump;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    dump.resize(size > 0 ? size_t(size) : 0);
    if (!dump.empty() && std::fread(dump.data(), 1, dump.size(), f) != dump.size()) dump.clear();
    std::fclose(f);
  }
  if (dump.size() < 0x400) {
    static std::atomic<uint32_t> warned{0};
    if (warned.fetch_add(1) < 4) REXLOG_WARN("[verify] read-only data: no image dump {}", path);
    return;
  }
  // The words the loader rewrites: the module's import records.
  std::vector<uint32_t> records;
  for (const auto& library : *module->xex_module()->import_libraries()) {
    for (const auto& import : library.imports) {
      if (import.value_address) records.push_back(import.value_address & ~3u);
      if (import.thunk_address) records.push_back(import.thunk_address & ~3u);
    }
  }
  std::sort(records.begin(), records.end());
  const size_t pe = Le32(dump, 0x3C);
  const uint32_t sections = Le32(dump, pe + 6) & 0xFFFF, optional = Le32(dump, pe + 20) & 0xFFFF;
  const uint32_t base = Le32(dump, pe + 24 + 28);
  const size_t table = pe + 24 + optional;
  auto* memory = rex::system::kernel_state()->memory();
  uint64_t compared = 0, different = 0;
  uint32_t first = 0;
  for (uint32_t i = 0; i < sections; ++i) {
    const size_t at = table + 40 * i;
    if (at + 40 > dump.size() || std::memcmp(&dump[at], ".rdata\0\0", 8) != 0) continue;
    const uint32_t size = Le32(dump, at + 8), va = Le32(dump, at + 12);
    if (uint64_t(va) + size > dump.size()) continue;
    const uint8_t* live = memory->TranslateVirtual<const uint8_t*>(base + va);
    for (uint32_t o = 0; o < size; ++o) {
      if (live[o] == dump[va + o]) continue;
      const uint32_t address = base + va + o;
      if (std::binary_search(records.begin(), records.end(), address & ~3u)) continue;
      if (!different++) first = address;
    }
    compared += size;
  }
  futari::native::ReportVerify("read-only data bytes", compared, different);
  if (different) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 8) {
      REXLOG_ERROR("[verify] DIFFERENCE: read-only data of {}: {} of {} bytes differ from the image dump {} (first "
                   "at {:08X}): a folded constant there would be wrong on the console", module->name(), different,
                   compared, path, first);
    }
  }
}

}  // namespace

extern "C" void RexRoMismatch(unsigned address) noexcept { RoMismatch("default", address); }
extern "C" void RexR1Mismatch(unsigned long long expected, unsigned long long got) noexcept {
  R1Mismatch("default", expected, got);
}
extern "C" void RexLrWriteMismatch(unsigned long long held, unsigned long long written) noexcept {
  LrWriteMismatch("default", held, written);
}
extern "C" void RexPromotedMismatch(unsigned target) noexcept { PromotedMismatch("default", target); }
extern "C" void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept {
  Census("default", function, site, target);
}

namespace futari::native {

void CodegenChecksInit() {
  g_rex_indirect_census = REXCVAR_GET(futari_diag_indirect_census).empty() ? 0 : 1;
  if (g_rex_indirect_census) {
    REXLOG_INFO("[census] indirect calls: every tagged call site reports its target; the table goes to {} with each "
                "report", REXCVAR_GET(futari_diag_indirect_census));
  }
}

void CodegenChecksReport(int seconds) {
  // What this run executed (differences are reported where they happen).
  static uint64_t reported_ro = 0, reported_promoted = 0, reported_pcr = 0, reported_pcr_wrong = 0;
  static uint64_t reported_r1 = 0, reported_lr = 0;
  const uint64_t ro = g_rex_ro_checked, promoted = g_rex_promoted_calls;
  const uint64_t r1 = g_rex_r1_checked, lr = g_rex_lr_write_checked;
  const uint64_t pcr = rex::syscount::CensusRead(rex::syscount::kCensusCritSecPcrOwner);
  const uint64_t pcr_wrong = rex::syscount::CensusRead(rex::syscount::kCensusCritSecOwnerMismatch);
  if (ro > reported_ro) ReportVerify("read-only constants", ro - reported_ro, 0);
  if (promoted > reported_promoted) ReportVerify("promoted indirect calls", promoted - reported_promoted, 0);
  if (r1 > reported_r1) ReportVerify("r1 mirror", r1 - reported_r1, 0);
  if (lr > reported_lr) ReportVerify("link-register write-backs", lr - reported_lr, 0);
  reported_r1 = r1;
  reported_lr = lr;
  {
    // tools/fp_sign_ops.py needs round-up / round-down never to be selected (cumulative, every module).
    using namespace rex::syscount;
    const uint64_t up_down = CensusRead(kCensusRoundUpDown);
    static uint64_t logged_up_down = 0;
    static int since = 0;
    since += seconds;
    if (since >= 60 || up_down != logged_up_down) {
      since = 0;
      if (up_down) {
        REXLOG_ERROR("[census] FPSCR rounding mode written by the guest (cumulative): to nearest {}, toward zero {}, "
                     "up or down {} (DIFFERENCE: tools/fp_sign_ops.py assumes never)",
                     CensusRead(kCensusRoundNearest), CensusRead(kCensusRoundZero), up_down);
      } else {
        REXLOG_INFO("[census] FPSCR rounding mode written by the guest (cumulative): to nearest {}, toward zero {}, "
                    "up or down 0", CensusRead(kCensusRoundNearest), CensusRead(kCensusRoundZero));
      }
      logged_up_down = up_down;
    }
  }
  if (pcr > reported_pcr || pcr_wrong > reported_pcr_wrong) {
    ReportVerify("critical section owners", pcr - reported_pcr, pcr_wrong - reported_pcr_wrong);
    if (pcr_wrong > reported_pcr_wrong) {
      REXLOG_ERROR("[verify] DIFFERENCE: critical section owners from the PCR: {} of {} were not the current thread",
                   pcr_wrong, pcr);
    }
  }
  reported_ro = ro;
  reported_promoted = promoted;
  reported_pcr = pcr;
  reported_pcr_wrong = pcr_wrong;
  if (const std::string folder = REXCVAR_GET(futari_diag_rdata_check); !folder.empty()) {
    auto* kernel = rex::system::kernel_state();
    CheckReadOnlyData(kernel->GetExecutableModule().get(), folder);
    for (const char* core : {"palm_normal.xex", "palm_abnormal.xex", "reco_normal.xex", "reco_abnormal.xex",
                             "hirow_normal.xex", "hirow_abnormal.xex", "kiniro_normal.xex", "kiniro_abnormal.xex"}) {
      if (auto module = kernel->GetModule(core, true)) {
        CheckReadOnlyData(dynamic_cast<rex::system::UserModule*>(module.get()), folder);
      }
    }
  }
  if (g_rex_indirect_census) WriteCensus(seconds);
}

}  // namespace futari::native

// For the game cores (futari_native_core.cpp): DLLs on the PC, with their own counters and the same reports.
#if defined(_WIN32)
#define FUTARI_CHECKS_EXPORT extern "C" __declspec(dllexport)
#else
#define FUTARI_CHECKS_EXPORT extern "C" __attribute__((visibility("default")))
#endif
FUTARI_CHECKS_EXPORT bool FutariIndirectCensusOn() { return g_rex_indirect_census != 0; }
FUTARI_CHECKS_EXPORT void FutariCoreIndirectCensus(const char* module, unsigned function, unsigned site,
                                                               unsigned target) {
  Census(module, function, site, target);
}
FUTARI_CHECKS_EXPORT void FutariCoreRoMismatch(const char* module, unsigned address) {
  RoMismatch(module, address);
}
FUTARI_CHECKS_EXPORT void FutariCorePromotedMismatch(const char* module, unsigned target) {
  PromotedMismatch(module, target);
}
FUTARI_CHECKS_EXPORT void FutariCoreR1Mismatch(const char* module, unsigned long long expected, unsigned long long got) {
  R1Mismatch(module, expected, got);
}
FUTARI_CHECKS_EXPORT void FutariCoreLrWriteMismatch(const char* module, unsigned long long held,
                                                    unsigned long long written) {
  LrWriteMismatch(module, held, written);
}

#else  // the Switch build: nothing to check, nothing to count

namespace futari::native {
void CodegenChecksInit() {}
void CodegenChecksReport(int) {}
}  // namespace futari::native

#endif
