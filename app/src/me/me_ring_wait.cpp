// Mass Effect - the game's D3D wait for the GPU without spinning (native renderer).
//
// WHY (Switch stack profile)
//   sub_8222FA98 is the D3D check "has the GPU read pointer moved since the last iteration":
//   32 db16cyc delays, then it compares the word the GPU writes the
//   read pointer back to ([[device+10768]]) with the value seen last time ([r3+8]), and after 5000 ticks
//   without change declares the GPU hung. Its callers loop on it. On the console it was ~22 % of the busy
//   game thread (plus ~6 % in the caller), on a 3-core machine where the ring thread needs that core.
//
// WHAT IT DOES
//   If the read pointer is unchanged, it waits at most futari_wait_ring_max_us for the ring thread
//   to write it back again (NotifyRingProgress), then calls the original, which decides as before. The game
//   sees the same values, only later. With bit 0x04 of [device+10813] (GPU already declared hung) or
//   without the native renderer, it does not wait.

#include "me_native_system.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include "renut_engine/futari_renut_yield.h"
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

REXCVAR_DEFINE_BOOL(futari_wait_blocking_ring, false, "Futari",
                    "Native renderer: the game's D3D sleeps while waiting for the ring thread to advance "
                    "instead of spinning in sub_8222FA98")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_notify_no_lock, false, "Futari",
                    "Ring thread: NotifyRingProgress takes the progress mutex only when the game's D3D wait is "
                    "registered as sleeping (sequentially consistent handshake on both sides) instead of on every call "
                    "(one per fence write and per ring segment). Same wake-ups")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(futari_wait_ring_max_us, 2000, "Futari",
                     "Native renderer: longest sleep per iteration of the game's D3D GPU wait, in microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

std::mutex g_progress_mutex;
std::condition_variable g_progress_cv;
std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiting{0};

}  // namespace

// The ring thread bumps the counter after writing the read pointer back, and checks for waiters under the
// lock (an unlocked Dekker handshake lost wake-ups on ARM).
void NotifyRingProgress() {
  static const bool no_lock = REXCVAR_GET(futari_notify_no_lock);
  if (no_lock) {
    // Dekker handshake, sequentially consistent on both sides: the waiter registers in g_waiting and then checks
    // g_progress (hook below); this side bumps g_progress and then reads g_waiting. One of the two sees the other's
    // write. If a waiter is registered, the lock/unlock below waits for it to be inside the condition variable (it holds
    // the mutex from registering until wait_for releases it), so the notify cannot be lost.
    g_progress.fetch_add(1, std::memory_order_seq_cst);
    if (g_waiting.load(std::memory_order_seq_cst) <= 0) return;
    { std::lock_guard<std::mutex> lock(g_progress_mutex); }
    g_progress_cv.notify_all();
    return;
  }
  g_progress.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) g_progress_cv.notify_all();
}

}  // namespace me::native

// banjo-nx: Mass Effect's hooks on its D3D busy-waits (sub_8222FA98 ring space, sub_822FE760 render-thread
// handoff, sub_82811750 occlusion poll) were here, with its device offsets (+10768 read-pointer word, +10813
// state). Banjo's equivalents are still to be identified (its device layout is 280 bytes further on for the
// shader fields: +12696 vs +12416), so only NotifyRingProgress above is kept.

// futari-nx: Mushihimesama Futari's D3D GPU wait, the same XDK code as Mass Effect's sub_8222FA98 at other device
// offsets (+10896 read-pointer word, +10941 state; bit 0x02 = GPU declared hung). default.xex sub_820A4590 and one copy
// per game core (futari_native_core.cpp). PC profile at 60 Hz: ~96 % of the game thread spun here (sub_820A4590 with
// its callers and helpers) while real game work was ~3 %; on the Switch the game thread showed 86-99 % CPU.
// futari_wait_blocking_ring (a row of futari_enhancements.cpp) turns it on; the wait only changes WHEN the game
// thread looks again, never what it sees.
namespace me::native {

namespace {
// Every 10 s: "[native] D3D GPU wait: N calls, ..." (what the wait did; masseffect-nx's Report()).
std::atomic<uint64_t> g_wait_calls{0}, g_wait_moved{0}, g_wait_other{0}, g_wait_sleeps{0}, g_wait_advanced{0},
    g_wait_ns{0};
std::atomic<int64_t> g_wait_next_report{0};
void ReportWaits() {
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t next = g_wait_next_report.load(std::memory_order_relaxed);
  if (now < next || !g_wait_next_report.compare_exchange_strong(next, now + 10)) return;
  if (!next) return;  // first call: start the interval
  REXLOG_INFO("[native] D3D GPU wait (10 s): {} checks, {} saw the ring moved, {} not applicable, {} slept ({} woken by "
              "ring progress), {:.1f} ms asleep",
              g_wait_calls.exchange(0), g_wait_moved.exchange(0), g_wait_other.exchange(0), g_wait_sleeps.exchange(0),
              g_wait_advanced.exchange(0), double(g_wait_ns.exchange(0)) / 1e6);
}
}  // namespace

void WaitD3DGpuProgress(uint8_t* base, uint32_t wait) {
  static const bool active = Enabled() && REXCVAR_GET(futari_wait_blocking_ring);
  if (!active || !wait) return;
  g_wait_calls.fetch_add(1, std::memory_order_relaxed);
  ReportWaits();
  constexpr uint32_t kOffReadPointerWord = 10896, kOffState = 10941;
  // As the generated code addresses guest memory (REX_RAW_ADDR): the PC keeps the physical views 0x1000 higher, and
  // the read-pointer write-back word is in them.
  const auto host = [base](uint32_t a) {
#if defined(_WIN32)
    return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);
#else
    return base + a;
#endif
  };
  const auto load32 = [host](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, host(a), 4);
    return __builtin_bswap32(v);
  };
  const uint32_t device = load32(wait);
  if (!device || (*host(device + kOffState) & 0x02)) {
    g_wait_other.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t word = load32(device + kOffReadPointerWord);
  if (!word) {
    g_wait_other.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t seen = g_progress.load(std::memory_order_acquire);
  if (load32(word) != load32(wait + 8)) {  // moved since the last look: the original handles it at once
    g_wait_moved.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const auto before = std::chrono::steady_clock::now();
  bool advanced;
  {
    std::unique_lock<std::mutex> lock(g_progress_mutex);
    g_waiting.fetch_add(1, std::memory_order_seq_cst);  // seq_cst: pairs with the lock-free notifier
    advanced = g_progress_cv.wait_for(lock, std::chrono::microseconds(REXCVAR_GET(futari_wait_ring_max_us)),
                                      [seen] { return g_progress.load(std::memory_order_seq_cst) != seen; });
    g_waiting.fetch_sub(1, std::memory_order_acq_rel);
  }
  g_wait_sleeps.fetch_add(1, std::memory_order_relaxed);
  if (advanced) g_wait_advanced.fetch_add(1, std::memory_order_relaxed);
  g_wait_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - before).count()),
                      std::memory_order_relaxed);
}

}  // namespace me::native

#if defined(_WIN32)
extern "C" __declspec(dllexport) void FutariWaitD3DGpuProgress(uint8_t* base, uint32_t wait) {
#else
extern "C" __attribute__((visibility("default"))) void FutariWaitD3DGpuProgress(uint8_t* base, uint32_t wait) {
#endif
  me::native::WaitD3DGpuProgress(base, wait);
}

REX_EXTERN(__imp__sub_820A4590);
REX_HOOK_RAW(sub_820A4590) {
  FUTARI_RENUT_YIELD(D3D_CBlocker_Check)  // Renut's renderer: its own wait (src/renut_engine)
  // The code check of the framework (src/native/futari_native.h): only the bytes this was written for.
  static const bool same_code = [base] {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < 54 * 4; ++i) h = (h ^ base[0x820A4590u + i]) * 0x100000001b3ull;
    if (h != 0x0069EB24868C4653ull) {
      REXLOG_WARN("[native] D3D GPU wait at 820A4590: guest code differs, the game spins as before");
      return false;
    }
    return true;
  }();
  if (same_code) me::native::WaitD3DGpuProgress(base, ctx.r3.u32);
  __imp__sub_820A4590(ctx, base);
}
