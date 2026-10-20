// Native replacements: a game core's side of the framework (futari_native.h). Compiled into every core target; the
// verify totals and the decision log live in the executable (futari_native.cpp), reached through its exports on the
// PC (the core is a DLL) and through the final link on the Switch (the core is a partially linked object whose own
// symbols are local).

#include "futari_native.h"

#include <atomic>
#include "../me/me_native_system.h"
#include "../me/futari_api.h"
// Renut's renderer (src/renut_engine/futari_renut_core.h): the core's own hooks of a function it replaces hand over.
#if FUTARI_RENUT_RENDERER
#include "../renut_engine/futari_renut_core.h"
#else
#define FUTARI_RENUT_CORE_YIELD(name)
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
extern "C" __attribute__((visibility("default"))) void FutariWaitD3DGpuProgress(uint8_t* base, uint32_t wait);
extern "C" __attribute__((visibility("default"))) void FutariNativeVerifyReport(const char* what, uint64_t checks,
                                                                                uint64_t differences);
extern "C" __attribute__((visibility("default"))) void FutariNativeDecided(const char* what, uint32_t address,
                                                                           int mode, uint64_t found_hash,
                                                                           uint64_t expected_hash);
extern "C" __attribute__((visibility("default"))) void FutariBeginDrawDirect(const uint8_t* base, uint32_t device,
                                                                             uint32_t end_offset,
                                                                             me::native::DrawDirect* out);
extern "C" __attribute__((visibility("default"))) void FutariEndDrawDirect(uint8_t* base,
                                                                           const me::native::DrawDirect* d);
extern "C" __attribute__((visibility("default"))) bool FutariApiBeginVertices(const futari::api::Module* module,
                                                                              PPCContext* ctx, uint8_t* base);
extern "C" __attribute__((visibility("default"))) bool FutariApiShadowInlineAlloc(PPCContext* ctx, uint8_t* base);
extern "C" __attribute__((visibility("default"))) bool FutariApiClear(const futari::api::Module* module, PPCContext* ctx,
                                                                      uint8_t* base);
extern "C" __attribute__((visibility("default"))) bool FutariApiResolve(const futari::api::Module* module,
                                                                        PPCContext* ctx, uint8_t* base);
extern "C" __attribute__((visibility("default"))) bool FutariApiSpaceDuringRedirect(PPCContext* ctx, uint8_t* base);
extern "C" __attribute__((visibility("default"))) void FutariApiNoteInlineAlloc(uint32_t address, uint32_t words);
extern "C" __attribute__((visibility("default"))) bool FutariApiRedirectedInlineAlloc(PPCContext* ctx, uint8_t* base,
                                                                                    futari::api::GuestFn original);
extern "C" __attribute__((visibility("default"))) bool FutariApiWindowScissor(const futari::api::Module* module,
                                                                              PPCContext* ctx, uint8_t* base);
extern "C" __attribute__((visibility("default"))) bool FutariCensusOn();
extern "C" __attribute__((visibility("default"))) void FutariCensusEnter(uint32_t address, const uint8_t* base,
                                                                         const char* module);
extern "C" __attribute__((visibility("default"))) void FutariCensusLeave(const uint8_t* base);
extern "C" __attribute__((visibility("default"))) void FutariCensusSegment(const uint8_t* base, int after);
extern "C" __attribute__((visibility("default"))) void FutariCensusNoteDraw(const uint8_t* base, uint32_t device);
#endif

#if !defined(__SWITCH__)
// The PC forms of the post-codegen steps count themselves and report a failed assumption (tools/flush_mode_flow.py,
// tools/ro_constants.py, tools/indirect_calls.py; the executable's side is futari_codegen_checks.cpp). A core is a
// DLL there: its own counters, and the reports go to the executable (CoreCodegen* below, once the core's name is
// known).
namespace futari::native {
void ReportVerify(const char* what, uint64_t checks, uint64_t differences);
}
extern "C" {
volatile unsigned long long g_rex_flush_flow_checked = 0;
volatile unsigned long long g_rex_ro_checked = 0;
volatile unsigned long long g_rex_promoted_calls = 0;
volatile unsigned long long g_rex_r1_checked = 0;        // banjo-nx's round: tools/r1_local.py (REX_R1_CHECK)
volatile unsigned long long g_rex_lr_write_checked = 0;  // and tools/lr_preserve.py (REX_LR_WRITE_SAME_CHECK)
// On until the first tagged call has asked the executable (the DLL may be loaded before the config is read).
unsigned char g_rex_indirect_census = 1;
void RexFlushFlowMismatch(int) noexcept { futari::native::ReportVerify("flush-mode flow (core)", 0, 1); }
}
#if !defined(_WIN32)
extern "C" __attribute__((visibility("default"))) bool FutariIndirectCensusOn();
extern "C" __attribute__((visibility("default"))) void FutariCoreIndirectCensus(const char* module, unsigned function,
                                                                                unsigned site, unsigned target);
extern "C" __attribute__((visibility("default"))) void FutariCoreRoMismatch(const char* module, unsigned address);
extern "C" __attribute__((visibility("default"))) void FutariCorePromotedMismatch(const char* module, unsigned target);
extern "C" __attribute__((visibility("default"))) void FutariCoreR1Mismatch(const char* module, unsigned long long expected,
                                                                            unsigned long long got);
extern "C" __attribute__((visibility("default"))) void FutariCoreLrWriteMismatch(const char* module,
                                                                                 unsigned long long held,
                                                                                 unsigned long long written);
#endif
#endif

namespace futari::native {

#if defined(_WIN32)
namespace {
template <typename T>
T FromExecutable(const char* name) {
  return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), name)));
}
}  // namespace

void ReportVerify(const char* what, uint64_t checks, uint64_t differences) {
  static const auto report = FromExecutable<void (*)(const char*, uint64_t, uint64_t)>("FutariNativeVerifyReport");
  if (report) {
    report(what, checks, differences);
    // The core's own counts of the generated code's self-checks ride along (a core has no periodic report of its
    // own): every 1024th report, so a verify session shows them.
    static std::atomic<uint32_t> calls{0};
    if ((calls.fetch_add(1, std::memory_order_relaxed) & 1023) == 0) {
      static std::atomic<unsigned long long> flow{0}, ro{0}, promoted{0}, r1{0}, lr{0};
      const unsigned long long now_flow = g_rex_flush_flow_checked, now_ro = g_rex_ro_checked,
                               now_promoted = g_rex_promoted_calls, now_r1 = g_rex_r1_checked,
                               now_lr = g_rex_lr_write_checked;
      const unsigned long long was_flow = flow.exchange(now_flow), was_ro = ro.exchange(now_ro),
                               was_promoted = promoted.exchange(now_promoted), was_r1 = r1.exchange(now_r1),
                               was_lr = lr.exchange(now_lr);
      if (now_flow > was_flow) report("flush-mode flow (core)", now_flow - was_flow, 0);
      if (now_ro > was_ro) report("read-only constants (core)", now_ro - was_ro, 0);
      if (now_promoted > was_promoted) report("promoted indirect calls (core)", now_promoted - was_promoted, 0);
      if (now_r1 > was_r1) report("r1 mirror (core)", now_r1 - was_r1, 0);
      if (now_lr > was_lr) report("link-register write-backs (core)", now_lr - was_lr, 0);
    }
  }
}

void ReportDecided(const char* what, uint32_t address, int mode, uint64_t found_hash, uint64_t expected_hash) {
  static const auto decided =
      FromExecutable<void (*)(const char*, uint32_t, int, uint64_t, uint64_t)>("FutariNativeDecided");
  if (decided) {
    decided(what, address, mode, found_hash, expected_hash);
  }
}

// The D3D GPU wait's sleep until the ring thread advances (src/me/me_ring_wait.cpp).
void WaitD3DGpuProgress(uint8_t* base, uint32_t wait) {
  static const auto wait_progress = FromExecutable<void (*)(uint8_t*, uint32_t)>("FutariWaitD3DGpuProgress");
  if (wait_progress) {
    wait_progress(base, wait);
  }
}

// futari_cpu_direct_state around a draw function (src/me/me_native_system.cpp, namespace direct).
void BeginDrawDirect(const uint8_t* base, uint32_t device, uint32_t end_offset, me::native::DrawDirect* out) {
  static const auto begin =
      FromExecutable<void (*)(const uint8_t*, uint32_t, uint32_t, me::native::DrawDirect*)>("FutariBeginDrawDirect");
  if (begin) {
    begin(base, device, end_offset, out);
  }
}

void EndDrawDirect(uint8_t* base, const me::native::DrawDirect* d) {
  static const auto end = FromExecutable<void (*)(uint8_t*, const me::native::DrawDirect*)>("FutariEndDrawDirect");
  if (end) {
    end(base, d);
  }
}

// The native Direct3D layer (src/me/futari_api.h).
bool ApiBeginVertices(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  static const auto f =
      FromExecutable<bool (*)(const futari::api::Module*, PPCContext*, uint8_t*)>("FutariApiBeginVertices");
  return f && f(&module, &ctx, base);
}
bool ApiShadowInlineAlloc(PPCContext& ctx, uint8_t* base) {
  static const auto f = FromExecutable<bool (*)(PPCContext*, uint8_t*)>("FutariApiShadowInlineAlloc");
  return f && f(&ctx, base);
}
bool ApiWindowScissor(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  static const auto f =
      FromExecutable<bool (*)(const futari::api::Module*, PPCContext*, uint8_t*)>("FutariApiWindowScissor");
  return f && f(&module, &ctx, base);
}
bool ApiClear(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  static const auto f = FromExecutable<bool (*)(const futari::api::Module*, PPCContext*, uint8_t*)>("FutariApiClear");
  return f && f(&module, &ctx, base);
}
bool ApiResolve(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  static const auto f = FromExecutable<bool (*)(const futari::api::Module*, PPCContext*, uint8_t*)>("FutariApiResolve");
  return f && f(&module, &ctx, base);
}
bool ApiSpaceDuringRedirect(PPCContext& ctx, uint8_t* base) {
  static const auto f = FromExecutable<bool (*)(PPCContext*, uint8_t*)>("FutariApiSpaceDuringRedirect");
  return f && f(&ctx, base);
}
void ApiNoteInlineAlloc(uint32_t address, uint32_t words) {
  static const auto f = FromExecutable<void (*)(uint32_t, uint32_t)>("FutariApiNoteInlineAlloc");
  if (f) f(address, words);
}
bool ApiRedirectedInlineAlloc(PPCContext& ctx, uint8_t* base, futari::api::GuestFn original) {
  static const auto f =
      FromExecutable<bool (*)(PPCContext*, uint8_t*, futari::api::GuestFn)>("FutariApiRedirectedInlineAlloc");
  return f && f(&ctx, base, original);
}

// The Direct3D census (src/me/futari_d3d_census.h).
bool CensusOn() {
  static const bool on = [] {
    const auto f = FromExecutable<bool (*)()>("FutariCensusOn");
    return f && f();
  }();
  return on;
}
void CensusEnter(uint32_t address, const uint8_t* base, const char* module) {
  static const auto f = FromExecutable<void (*)(uint32_t, const uint8_t*, const char*)>("FutariCensusEnter");
  if (f) f(address, base, module);
}
void CensusLeave(const uint8_t* base) {
  static const auto f = FromExecutable<void (*)(const uint8_t*)>("FutariCensusLeave");
  if (f) f(base);
}
void CensusSegment(const uint8_t* base, int after) {
  static const auto f = FromExecutable<void (*)(const uint8_t*, int)>("FutariCensusSegment");
  if (f) f(base, after);
}
void CensusNoteDraw(const uint8_t* base, uint32_t device) {
  static const auto f = FromExecutable<void (*)(const uint8_t*, uint32_t)>("FutariCensusNoteDraw");
  if (f) f(base, device);
}
#else
void BeginDrawDirect(const uint8_t* base, uint32_t device, uint32_t end_offset, me::native::DrawDirect* out) {
  FutariBeginDrawDirect(base, device, end_offset, out);
}

void EndDrawDirect(uint8_t* base, const me::native::DrawDirect* d) { FutariEndDrawDirect(base, d); }

bool ApiBeginVertices(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  return FutariApiBeginVertices(&module, &ctx, base);
}
bool ApiShadowInlineAlloc(PPCContext& ctx, uint8_t* base) { return FutariApiShadowInlineAlloc(&ctx, base); }
bool ApiWindowScissor(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  return FutariApiWindowScissor(&module, &ctx, base);
}
bool ApiClear(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  return FutariApiClear(&module, &ctx, base);
}
bool ApiResolve(const futari::api::Module& module, PPCContext& ctx, uint8_t* base) {
  return FutariApiResolve(&module, &ctx, base);
}
bool ApiSpaceDuringRedirect(PPCContext& ctx, uint8_t* base) { return FutariApiSpaceDuringRedirect(&ctx, base); }
void ApiNoteInlineAlloc(uint32_t address, uint32_t words) { FutariApiNoteInlineAlloc(address, words); }
bool ApiRedirectedInlineAlloc(PPCContext& ctx, uint8_t* base, futari::api::GuestFn original) {
  return FutariApiRedirectedInlineAlloc(&ctx, base, original);
}

bool CensusOn() {
  static const bool on = FutariCensusOn();
  return on;
}
void CensusEnter(uint32_t address, const uint8_t* base, const char* module) {
  FutariCensusEnter(address, base, module);
}
void CensusLeave(const uint8_t* base) { FutariCensusLeave(base); }
void CensusSegment(const uint8_t* base, int after) { FutariCensusSegment(base, after); }
void CensusNoteDraw(const uint8_t* base, uint32_t device) { FutariCensusNoteDraw(base, device); }

void WaitD3DGpuProgress(uint8_t* base, uint32_t wait) { FutariWaitD3DGpuProgress(base, wait); }

void ReportVerify(const char* what, uint64_t checks, uint64_t differences) {
  FutariNativeVerifyReport(what, checks, differences);
}

void ReportDecided(const char* what, uint32_t address, int mode, uint64_t found_hash, uint64_t expected_hash) {
  FutariNativeDecided(what, address, mode, found_hash, expected_hash);
}
#endif

}  // namespace futari::native

// ---------------------------------------------------------------------------------------------------------------------
// The replacements themselves. Each core defines FUTARI_CORE_<name> (app/CMakeLists.txt); a function shared by the
// cores sits at a per-core address (tools/find_same_functions.py finds the copies, tools/native_code_hash.py checks
// they are the same bytes).

#include <rex/hook.h>

#include "futari_native_crt.h"

#define FUTARI_NATIVE_JOIN2(a, b) a##b
#define FUTARI_NATIVE_JOIN(a, b) FUTARI_NATIVE_JOIN2(a, b)
#define FUTARI_NATIVE_HEX2(x) 0x##x
#define FUTARI_NATIVE_HEX(x) FUTARI_NATIVE_HEX2(x)

#if defined(FUTARI_CORE_palm_normal)
#define FUTARI_CORE_NAME "palm_normal"
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_NAME "palm_abnormal"
#elif defined(FUTARI_CORE_reco_normal)
#define FUTARI_CORE_NAME "reco_normal"
#elif defined(FUTARI_CORE_reco_abnormal)
#define FUTARI_CORE_NAME "reco_abnormal"
#elif defined(FUTARI_CORE_hirow_normal)
#define FUTARI_CORE_NAME "hirow_normal"
#elif defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_NAME "hirow_abnormal"
#elif defined(FUTARI_CORE_kiniro_normal)
#define FUTARI_CORE_NAME "kiniro_normal"
#elif defined(FUTARI_CORE_kiniro_abnormal)
#define FUTARI_CORE_NAME "kiniro_abnormal"
#endif

// memcpy (futari_native_crt.h): default.xex 8207DAC0, the cores below.
#if !defined(__SWITCH__)
// The generated code's self-checks of this core (see the top of the file): to the executable, under the core's name.
extern "C" {
void RexRoMismatch(unsigned address) noexcept {
#if defined(_WIN32)
  static const auto report = reinterpret_cast<void (*)(const char*, unsigned)>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "FutariCoreRoMismatch")));
  if (report) report(FUTARI_CORE_NAME, address);
#else
  FutariCoreRoMismatch(FUTARI_CORE_NAME, address);
#endif
}
void RexPromotedMismatch(unsigned target) noexcept {
#if defined(_WIN32)
  static const auto report = reinterpret_cast<void (*)(const char*, unsigned)>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "FutariCorePromotedMismatch")));
  if (report) report(FUTARI_CORE_NAME, target);
#else
  FutariCorePromotedMismatch(FUTARI_CORE_NAME, target);
#endif
}
void RexR1Mismatch(unsigned long long expected, unsigned long long got) noexcept {
#if defined(_WIN32)
  static const auto report = reinterpret_cast<void (*)(const char*, unsigned long long, unsigned long long)>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "FutariCoreR1Mismatch")));
  if (report) report(FUTARI_CORE_NAME, expected, got);
#else
  FutariCoreR1Mismatch(FUTARI_CORE_NAME, expected, got);
#endif
}
void RexLrWriteMismatch(unsigned long long held, unsigned long long written) noexcept {
#if defined(_WIN32)
  static const auto report = reinterpret_cast<void (*)(const char*, unsigned long long, unsigned long long)>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "FutariCoreLrWriteMismatch")));
  if (report) report(FUTARI_CORE_NAME, held, written);
#else
  FutariCoreLrWriteMismatch(FUTARI_CORE_NAME, held, written);
#endif
}
void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept {
#if defined(_WIN32)
  static const auto on = reinterpret_cast<bool (*)()>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "FutariIndirectCensusOn")));
  static const auto census = reinterpret_cast<void (*)(const char*, unsigned, unsigned, unsigned)>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "FutariCoreIndirectCensus")));
  if (!on || !census || !on()) {
    g_rex_indirect_census = 0;  // the census is off: no further calls from this core
    return;
  }
  census(FUTARI_CORE_NAME, function, site, target);
#else
  if (!FutariIndirectCensusOn()) {
    g_rex_indirect_census = 0;
    return;
  }
  FutariCoreIndirectCensus(FUTARI_CORE_NAME, function, site, target);
#endif
}
}
#endif

#if defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_MEMCPY 88082260
#elif defined(FUTARI_CORE_palm_normal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_MEMCPY 88072270
#elif defined(FUTARI_CORE_kiniro_normal) || defined(FUTARI_CORE_kiniro_abnormal) || \
    defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_reco_abnormal)
#define FUTARI_CORE_MEMCPY 88072260
#endif

// memset (futari_native_crt.h): default.xex 8207DA20, the cores below (the same 40 instructions everywhere).
#if defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_MEMSET 88083BD0
#elif defined(FUTARI_CORE_palm_normal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_MEMSET 88073BE0
#elif defined(FUTARI_CORE_kiniro_normal) || defined(FUTARI_CORE_kiniro_abnormal) || \
    defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_reco_abnormal)
#define FUTARI_CORE_MEMSET 88073BD0
#endif

#if defined(FUTARI_CORE_MEMSET)
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_MEMSET));
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_MEMSET)) {
  static futari::native::Site site{"futari_native_memset", "memset (core)", FUTARI_NATIVE_HEX(FUTARI_CORE_MEMSET),
                                   futari::native::crt::kMemsetWords, futari::native::crt::kMemsetHash};
  futari::native::crt::MemsetSite<FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_MEMSET)>(site, ctx, base);
}
#endif

#if defined(FUTARI_CORE_MEMCPY)
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_MEMCPY));
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_MEMCPY)) {
  static futari::native::Site site{"futari_native_memcpy", "memcpy (core)", FUTARI_NATIVE_HEX(FUTARI_CORE_MEMCPY),
                                   futari::native::crt::kMemcpyWords, futari::native::crt::kMemcpyHash};
  futari::native::crt::MemcpySite<FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_MEMCPY)>(site, ctx, base);
}
#endif

// The D3D GPU wait (src/me/me_ring_wait.cpp): each core links its own Direct3D, with the same spin as default.xex's
// sub_820A4590 (same device offsets, 54 instructions, the same bytes in all eight cores).
#if defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_D3D_WAIT 88091300
#elif defined(FUTARI_CORE_palm_normal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_D3D_WAIT 88081310
#elif defined(FUTARI_CORE_kiniro_normal) || defined(FUTARI_CORE_kiniro_abnormal) ||     defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_reco_abnormal)
#define FUTARI_CORE_D3D_WAIT 88081300
#endif

#if defined(FUTARI_CORE_D3D_WAIT)
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_D3D_WAIT));
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_D3D_WAIT)) {
  FUTARI_RENUT_CORE_YIELD(D3D_CBlocker_Check)
  static const bool same_code =
      futari::native::CodeHash(base, FUTARI_NATIVE_HEX(FUTARI_CORE_D3D_WAIT), 54) == 0x709915D9682F53F4ull;
  if (same_code) futari::native::WaitD3DGpuProgress(base, ctx.r3.u32);
  FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_D3D_WAIT)(ctx, base);
}
#endif

// The LZSS decompressor (futari_native_lzss.h): the same 76 instructions in all eight cores.
#include "futari_native_lzss.h"

#if defined(FUTARI_CORE_reco_abnormal)
#define FUTARI_CORE_LZSS 8813A120
#elif defined(FUTARI_CORE_reco_normal)
#define FUTARI_CORE_LZSS 8813A118
#elif defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_LZSS 88137B38
#elif defined(FUTARI_CORE_hirow_normal)
#define FUTARI_CORE_LZSS 88137B30
#elif defined(FUTARI_CORE_kiniro_abnormal)
#define FUTARI_CORE_LZSS 8813D5C8
#elif defined(FUTARI_CORE_kiniro_normal)
#define FUTARI_CORE_LZSS 8813D5C0
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_LZSS 881166E8
#elif defined(FUTARI_CORE_palm_normal)
#define FUTARI_CORE_LZSS 8813BEC8
#endif

#if defined(FUTARI_CORE_LZSS)
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_LZSS));
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_LZSS)) {
  static futari::native::Site site{"futari_native_lzss", "LZSS decompress (core)", FUTARI_NATIVE_HEX(FUTARI_CORE_LZSS),
                                   futari::native::lzss::kWords, futari::native::lzss::kHash};
  futari::native::lzss::Site<FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_LZSS)>(site, ctx, base);
}
#endif

// futari_cpu_direct_state (src/me/me_native_system.cpp, namespace direct; default.xex's side in
// src/me/futari_direct_hooks.cpp): the core's own Direct3D BeginVertices (dev, primitive type, vertex count, stride),
// which the game's sprite drawing goes through (DrawVerticesUP calls it too). It flushes the dirty state as PM4, writes
// the draw packet and returns the vertex memory the caller fills, or 0 with no draw packet. The draw packet's last word
// is at device+13652 (EndVertices, sub_8807EC90 in reco_abnormal, copies it to device+48). The same 293 instructions
// in all eight cores.
#if defined(FUTARI_CORE_reco_abnormal) || defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_kiniro_abnormal) ||     defined(FUTARI_CORE_kiniro_normal)
#define FUTARI_CORE_BEGIN_VERTICES 8807ECA0
#elif defined(FUTARI_CORE_hirow_abnormal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_palm_normal)
#define FUTARI_CORE_BEGIN_VERTICES 8807ECB0
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_BEGIN_VERTICES 8808ECA0
#endif

// futari_api_draws (src/me/futari_api.cpp): the core's own Direct3D functions the native BeginVertices calls
// (tools/d3d_module_table.py: the same instructions as default.xex's, branch targets and immediates aside), written as
// __imp__ names only (a call of the original is not a hook: tools/perf_codegen_post.py keeps their calls direct), and
// its Direct3D globals (interpolator mask, its init flag, declaration ids; 12 bytes).
#if defined(FUTARI_CORE_palm_normal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_FN_ALLOC_INLINE __imp__sub_8807FE08
#define FUTARI_CORE_FN_SPACE __imp__sub_88080A10
#define FUTARI_CORE_FN_SHADER_LOAD __imp__sub_8807B748
#define FUTARI_CORE_FN_RB_RESEND __imp__sub_8807A1F0
#define FUTARI_CORE_FN_DEPTH_2203 __imp__sub_88079FF8
#define FUTARI_CORE_FN_BINDING_MATCH __imp__sub_8807B608
#define FUTARI_CORE_FN_BIND_INLINE __imp__sub_8807B370
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_FN_ALLOC_INLINE __imp__sub_8808FDF8
#define FUTARI_CORE_FN_SPACE __imp__sub_88090A00
#define FUTARI_CORE_FN_SHADER_LOAD __imp__sub_8808B738
#define FUTARI_CORE_FN_RB_RESEND __imp__sub_8808A1E0
#define FUTARI_CORE_FN_DEPTH_2203 __imp__sub_88089FE8
#define FUTARI_CORE_FN_BINDING_MATCH __imp__sub_8808B5F8
#define FUTARI_CORE_FN_BIND_INLINE __imp__sub_8808B360
#elif defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_reco_abnormal) || defined(FUTARI_CORE_kiniro_normal) || \
    defined(FUTARI_CORE_kiniro_abnormal)
#define FUTARI_CORE_FN_ALLOC_INLINE __imp__sub_8807FDF8
#define FUTARI_CORE_FN_SPACE __imp__sub_88080A00
#define FUTARI_CORE_FN_SHADER_LOAD __imp__sub_8807B738
#define FUTARI_CORE_FN_RB_RESEND __imp__sub_8807A1E0
#define FUTARI_CORE_FN_DEPTH_2203 __imp__sub_88079FE8
#define FUTARI_CORE_FN_BINDING_MATCH __imp__sub_8807B5F8
#define FUTARI_CORE_FN_BIND_INLINE __imp__sub_8807B360
#endif
#if defined(FUTARI_CORE_palm_normal)
#define FUTARI_CORE_D3D_GLOBALS 0x88362E78u
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_D3D_GLOBALS 0x88372AB8u
#elif defined(FUTARI_CORE_reco_normal)
#define FUTARI_CORE_D3D_GLOBALS 0x88352A38u
#elif defined(FUTARI_CORE_reco_abnormal)
#define FUTARI_CORE_D3D_GLOBALS 0x88352A98u
#elif defined(FUTARI_CORE_hirow_normal)
#define FUTARI_CORE_D3D_GLOBALS 0x88352518u
#elif defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_D3D_GLOBALS 0x88352578u
#elif defined(FUTARI_CORE_kiniro_normal)
#define FUTARI_CORE_D3D_GLOBALS 0x88345D58u
#elif defined(FUTARI_CORE_kiniro_abnormal)
#define FUTARI_CORE_D3D_GLOBALS 0x88345E58u
#endif
// The window offset / scissor writer (futari_api_window_scissor), hooked.
#if defined(FUTARI_CORE_palm_normal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_WINDOW_SCISSOR 88075408
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_WINDOW_SCISSOR 880853F8
#elif defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_reco_abnormal) || defined(FUTARI_CORE_kiniro_normal) || \
    defined(FUTARI_CORE_kiniro_abnormal)
#define FUTARI_CORE_WINDOW_SCISSOR 880753F8
#endif
// Clear, Resolve (futari_api_clear / futari_api_resolve) and the space function (no submission during a redirect),
// hooked.
#if defined(FUTARI_CORE_palm_normal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_CLEAR 8807D508
#define FUTARI_CORE_RESOLVE 8807DD98
#define FUTARI_CORE_SPACE 88080A10
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_CLEAR 8808D4F8
#define FUTARI_CORE_RESOLVE 8808DD88
#define FUTARI_CORE_SPACE 88090A00
#elif defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_reco_abnormal) || defined(FUTARI_CORE_kiniro_normal) || \
    defined(FUTARI_CORE_kiniro_abnormal)
#define FUTARI_CORE_CLEAR 8807D4F8
#define FUTARI_CORE_RESOLVE 8807DD88
#define FUTARI_CORE_SPACE 88080A00
#endif
// The inline-data allocator itself is hooked (verify mode's shadow runs answer from a scratch area).
#if defined(FUTARI_CORE_palm_normal) || defined(FUTARI_CORE_hirow_normal) || defined(FUTARI_CORE_hirow_abnormal)
#define FUTARI_CORE_ALLOC_INLINE 8807FE08
#elif defined(FUTARI_CORE_palm_abnormal)
#define FUTARI_CORE_ALLOC_INLINE 8808FDF8
#elif defined(FUTARI_CORE_reco_normal) || defined(FUTARI_CORE_reco_abnormal) || defined(FUTARI_CORE_kiniro_normal) || \
    defined(FUTARI_CORE_kiniro_abnormal)
#define FUTARI_CORE_ALLOC_INLINE 8807FDF8
#endif

#if defined(FUTARI_CORE_FN_ALLOC_INLINE)
extern "C" REX_FUNC(FUTARI_CORE_FN_ALLOC_INLINE);
extern "C" REX_FUNC(FUTARI_CORE_FN_SPACE);
extern "C" REX_FUNC(FUTARI_CORE_FN_SHADER_LOAD);
extern "C" REX_FUNC(FUTARI_CORE_FN_RB_RESEND);
extern "C" REX_FUNC(FUTARI_CORE_FN_DEPTH_2203);
extern "C" REX_FUNC(FUTARI_CORE_FN_BINDING_MATCH);
extern "C" REX_FUNC(FUTARI_CORE_FN_BIND_INLINE);
#endif

#if defined(FUTARI_CORE_ALLOC_INLINE)
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_ALLOC_INLINE)) {
  FUTARI_RENUT_CORE_YIELD(D3D_CDevice_AllocateInlineData)
  if (futari::native::ApiShadowInlineAlloc(ctx, base)) return;
  if (futari::native::ApiRedirectedInlineAlloc(ctx, base, &FUTARI_CORE_FN_ALLOC_INLINE)) return;
  const uint32_t words = ctx.r4.u32;
  FUTARI_CORE_FN_ALLOC_INLINE(ctx, base);
  futari::native::ApiNoteInlineAlloc(ctx.r3.u32, words);
}
#endif

#if defined(FUTARI_CORE_BEGIN_VERTICES) && defined(FUTARI_CORE_WINDOW_SCISSOR)
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_BEGIN_VERTICES));
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_WINDOW_SCISSOR));
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_CLEAR));
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_RESOLVE));
namespace {
// The core's own Direct3D functions the native layer calls (src/me/futari_api.h).
const futari::api::Module& CoreModule() {
  static const futari::api::Module module{FUTARI_CORE_NAME,
                                          &FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_BEGIN_VERTICES),
                                          &FUTARI_CORE_FN_ALLOC_INLINE,
                                          &FUTARI_CORE_FN_SPACE,
                                          &FUTARI_CORE_FN_SHADER_LOAD,
                                          &FUTARI_CORE_FN_RB_RESEND,
                                          &FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_WINDOW_SCISSOR),
                                          &FUTARI_CORE_FN_DEPTH_2203,
                                          &FUTARI_CORE_FN_BINDING_MATCH,
                                          &FUTARI_CORE_FN_BIND_INLINE,
                                          &FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_CLEAR),
                                          &FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_RESOLVE),
                                          FUTARI_CORE_D3D_GLOBALS};
  return module;
}
}  // namespace
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_CLEAR)) {
  FUTARI_RENUT_CORE_YIELD(D3DDevice_Clear)
  if (futari::native::ApiClear(CoreModule(), ctx, base)) return;
  FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_CLEAR)(ctx, base);
}
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_RESOLVE)) {
  FUTARI_RENUT_CORE_YIELD(D3DDevice_Resolve)
  if (futari::native::ApiResolve(CoreModule(), ctx, base)) return;
  FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_RESOLVE)(ctx, base);
}
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_SPACE)) {
  FUTARI_RENUT_CORE_YIELD(D3D_CDevice_KickOff)
  if (futari::native::ApiSpaceDuringRedirect(ctx, base)) return;
  FUTARI_CORE_FN_SPACE(ctx, base);
}
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_WINDOW_SCISSOR)) {
  FUTARI_RENUT_CORE_YIELD(D3D_SetSurfaceClip)
  if (futari::native::ApiWindowScissor(CoreModule(), ctx, base)) return;
  FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_WINDOW_SCISSOR)(ctx, base);
}
#endif

#if defined(FUTARI_CORE_BEGIN_VERTICES)
extern "C" REX_FUNC(FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_BEGIN_VERTICES));
REX_HOOK_RAW(FUTARI_NATIVE_JOIN(sub_, FUTARI_CORE_BEGIN_VERTICES)) {
  FUTARI_RENUT_CORE_YIELD(D3DDevice_BeginVertices)
  static const bool same_code = [base] {
    constexpr uint64_t kExpected = 0xAEA9EF7899734FDFull;
    const uint64_t found = futari::native::CodeHash(base, FUTARI_NATIVE_HEX(FUTARI_CORE_BEGIN_VERTICES), 293);
    if (found != kExpected) {
      futari::native::ReportDecided("BeginVertices (core, direct state)", FUTARI_NATIVE_HEX(FUTARI_CORE_BEGIN_VERTICES),
                                    -1, found, kExpected);
    }
    return found == kExpected;
  }();
  if (futari::native::CensusOn()) [[unlikely]] {  // futari_diag_d3d_census: counted, nothing converted
    futari::native::CensusNoteDraw(base, ctx.r3.u32);
    futari::native::CensusEnter(FUTARI_NATIVE_HEX(FUTARI_CORE_BEGIN_VERTICES), base, FUTARI_CORE_NAME);
    FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_BEGIN_VERTICES)(ctx, base);
    futari::native::CensusLeave(base);
    return;
  }
  if (!same_code) return FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_BEGIN_VERTICES)(ctx, base);
  if (futari::native::ApiBeginVertices(CoreModule(), ctx, base)) return;  // futari_api_draws
  me::native::DrawDirect direct;
  futari::native::BeginDrawDirect(base, ctx.r3.u32, 13652, &direct);
  FUTARI_NATIVE_JOIN(__imp__sub_, FUTARI_CORE_BEGIN_VERTICES)(ctx, base);
  if (!ctx.r3.u32) direct.device = 0;  // nothing allocated: no draw packet (device+13652 is an old draw's)
  futari::native::EndDrawDirect(base, &direct);
}
#endif
