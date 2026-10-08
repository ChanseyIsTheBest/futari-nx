// Mushihimesama Futari's frame pacing without spinning (futari_frame_wait_sleep).
//
// default.xex sub_8216B148, the game thread's per-frame step, waits for the next frame with two busy loops on a 64-bit
// counter at 0x821F8560: `while (counter == last) {}` (8216B244 and 8216B2E4). The counter is only written by
// sub_8216AD38 (`counter += 1`), the vertical-blank callback the guest's D3D calls 60 times a second. Console profile,
// 2026-10-06: 78.6 % of the busy game thread spun there, a whole core of a three-core machine.
//
// Two mid-function hooks (app/perf_overrides_default.toml, [[midasm_hook]] at the top of each loop) call
// FutariFrameWaitHook / FutariFrameWaitHook2 on every iteration. If the counter still holds the value the loop waits
// on, the game thread sleeps until the vblank callback below bumps it (or 2 ms pass); then the loop's own compare runs
// as before. The guest sees the same values, only after a sleep instead of a spin.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "futari_native.h"

REXCVAR_DEFINE_BOOL(futari_frame_wait_sleep, true, "Futari",
                    "The game thread sleeps until the next vertical blank instead of spinning on the frame counter "
                    "(sub_8216B148); same frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kCounter = 0x821F8560;  // -31392(0x82200000)

std::atomic<uint8_t*> g_base{nullptr};
std::atomic<uint64_t> g_bumps{0};
std::atomic<int> g_waiting{0};
std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<uint64_t> g_sleeps{0}, g_woken{0}, g_ns{0};

// The guest code the hooks were written for: the callback, and the two loops (ld / cmpld / beq).
bool CodeMatches(uint8_t* base) {
  static const bool matches = [base] {
    const auto word = [base](uint32_t a) {
      uint32_t v;
      std::memcpy(&v, base + a, 4);
      return __builtin_bswap32(v);
    };
    const bool callback = word(0x8216AD38) == 0x3D408220 && word(0x8216AD3C) == 0xE96A8560 &&
                          word(0x8216AD40) == 0x392B0001 && word(0x8216AD44) == 0xF92A8560;
    const bool loop1 = word(0x8216B244) == 0xE92B8560 && word(0x8216B248) == 0x7F295040;
    const bool loop2 = word(0x8216B2E4) == 0xE90A8560 && word(0x8216B2E8) == 0x7F285840;
    if (!(callback && loop1 && loop2)) {
      REXLOG_WARN("[native] frame wait: guest code differs ({} {} {}), the game spins as before", callback, loop1,
                  loop2);
      return false;
    }
    REXLOG_INFO("[native] frame wait: the game thread sleeps until the next vertical blank (futari_frame_wait_sleep)");
    return true;
  }();
  return matches;
}

uint64_t LoadCounter(const uint8_t* base) {
  uint64_t v;
  std::memcpy(&v, base + kCounter, 8);
  return __builtin_bswap64(v);
}

void WaitCounter(uint64_t last) {
  static const bool on = REXCVAR_GET(futari_frame_wait_sleep);
  uint8_t* base = g_base.load(std::memory_order_acquire);
  if (!on || !base || !CodeMatches(base)) return;  // until the callback has run once the loop spins as before
  const uint64_t seen = g_bumps.load(std::memory_order_seq_cst);
  if (LoadCounter(base) != last) return;
  const auto before = std::chrono::steady_clock::now();
  bool woken;
  {
    std::unique_lock lock(g_mutex);
    g_waiting.fetch_add(1, std::memory_order_seq_cst);
    woken = g_cv.wait_for(lock, std::chrono::milliseconds(2),
                          [seen] { return g_bumps.load(std::memory_order_seq_cst) != seen; });
    g_waiting.fetch_sub(1, std::memory_order_acq_rel);
  }
  g_sleeps.fetch_add(1, std::memory_order_relaxed);
  if (woken) g_woken.fetch_add(1, std::memory_order_relaxed);
  g_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - before).count()),
                 std::memory_order_relaxed);
}

}  // namespace

// The vertical-blank callback: the original increment, then wake the game thread.
REX_EXTERN(__imp__sub_8216AD38);
REX_HOOK_RAW(sub_8216AD38) {
  g_base.store(base, std::memory_order_release);
  __imp__sub_8216AD38(ctx, base);
  g_bumps.fetch_add(1, std::memory_order_seq_cst);
  if (g_waiting.load(std::memory_order_seq_cst) > 0) {
    { std::lock_guard lock(g_mutex); }
    g_cv.notify_all();
  }
  // Every 600 vertical blanks (10 s): what the wait did.
  const uint64_t n = g_bumps.load(std::memory_order_relaxed);
  if (n % 600 == 0 && g_sleeps.load(std::memory_order_relaxed)) {
    const uint64_t sleeps = g_sleeps.exchange(0), woken = g_woken.exchange(0), ns = g_ns.exchange(0);
    REXLOG_INFO("[native] frame wait (10 s): {} sleeps, {} woken by the vertical blank, {:.1f} ms asleep", sleeps,
                woken, double(ns) / 1e6);
  }
}

// [[midasm_hook]] 0x8216B244 (registers r10, r11): `ld r9,-31392(r11); cmpld r9,r10` -> last value in r10.
void FutariFrameWaitHook(PPCRegister& r10, PPCRegister& r11) {
  (void)r11;
  WaitCounter(r10.u64);
}

// [[midasm_hook]] 0x8216B2E4 (registers r10, r11): `ld r8,-31392(r10); cmpld r8,r11` -> last value in r11.
void FutariFrameWaitHook2(PPCRegister& r10, PPCRegister& r11) {
  (void)r10;
  WaitCounter(r11.u64);
}
