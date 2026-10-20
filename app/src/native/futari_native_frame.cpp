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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "futari_native.h"

REXCVAR_DEFINE_BOOL(futari_frame_wait_sleep, true, "Futari",
                    "The game thread sleeps until the next vertical blank instead of spinning on the frame counter "
                    "(sub_8216B148); same frames")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Input lag (2026-10-16). The game's frame step Swaps right after the vertical blank, reads the controllers at once
// (0.03 ms later on the PC), runs the frame in a few ms and then sleeps here until the next vblank: 13 ms of the 16.7 on
// the console (the 10 s "frame wait" line). So the image swapped at a vblank shows controllers read a whole frame
// earlier. With this key the frame's first controller read waits by what the frame can spare: the time to the next
// vblank, less the longest frame of the last 64 (measured from that read to the wait here), less a margin. (Not at
// VdSwap's return: the game kicks the Swap packet to the GPU after VdSwap, so the present moved with it.) A frame that
// needs more than that misses its vblank (shown one later, the "[latency]" line counts them) and shortens the next
// delays at once. Swap, vblank and the frame's own work are unchanged; only when the controllers are read moves.
REXCVAR_DEFINE_BOOL(futari_late_input, true, "Futari",
                    "Input lag: the game reads its controllers as late as its frame allows (its first read after "
                    "each Swap waits by the time the frame can spare); false = right after the Swap, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(futari_late_input_margin_us, 3000, "Futari",
                     "futari_late_input: microseconds kept free before the vertical blank beyond the longest recent "
                     "frame (larger = fewer missed frames, less gain)")
    .range(500, 16000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(futari_late_input_max_us, 12000, "Futari",
                     "futari_late_input: the longest delay, in microseconds")
    .range(0, 15000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr uint32_t kCounter = 0x821F8560;  // -31392(0x82200000)

std::atomic<uint8_t*> g_base{nullptr};
std::atomic<uint64_t> g_bumps{0};
std::atomic<int> g_waiting{0};
std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<uint64_t> g_sleeps{0}, g_woken{0}, g_ns{0};

// futari_late_input. Times in steady-clock nanoseconds.
uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
std::atomic<uint64_t> g_vblank_ns{0};        // the last vertical blank (the callback below)
std::atomic<uint64_t> g_period_ns{16666667};  // the vblank period, measured
std::atomic<uint64_t> g_resume_ns{0};         // when the game resumed after its last Swap (0: measured already)
constexpr uint32_t kWindow = 64;
uint64_t g_work_ns[kWindow] = {};            // the last frames' work, from the resume to the wait (game thread)
uint32_t g_work_next = 0;
uint32_t g_work_count = 0;
std::atomic<uint64_t> g_delay_sum_us{0}, g_delays{0}, g_work_max_report_ns{0};

uint64_t WorkMax() {
  uint64_t m = 0;
  for (uint32_t i = 0; i < g_work_count; ++i) m = std::max(m, g_work_ns[i]);
  return m;
}

// The frame's first controller read, on the game thread (SetFrameReadDelayFunction): right after the Swap.
uint32_t FrameReadDelay() {
  const uint64_t now = NowNs();
  static const uint64_t margin = uint64_t(REXCVAR_GET(futari_late_input_margin_us)) * 1000;
  static const uint64_t most = uint64_t(REXCVAR_GET(futari_late_input_max_us)) * 1000;
  const uint64_t vblank = g_vblank_ns.load(std::memory_order_acquire);
  const uint64_t period = g_period_ns.load(std::memory_order_relaxed);
  uint64_t delay = 0;
  // Only once a few frames are measured, and only when the Swap followed a vblank of this frame (a game running
  // late or at 30 frames a second gets no delay).
  if (vblank && g_work_count >= 8 && now >= vblank && now - vblank < period / 2) {
    const uint64_t next = vblank + period;
    const uint64_t need = WorkMax() + margin;
    if (next > now + need) delay = std::min(next - now - need, most);
  }
  g_resume_ns.store(now + delay, std::memory_order_release);
  if (delay) {
    g_delay_sum_us.fetch_add(delay / 1000, std::memory_order_relaxed);
    g_delays.fetch_add(1, std::memory_order_relaxed);
  }
  return uint32_t(delay / 1000);
}

// The game reached its wait for the next vblank: the frame's work since it resumed after the Swap.
void NoteFrameWork() {
  const uint64_t resumed = g_resume_ns.exchange(0, std::memory_order_acq_rel);
  if (!resumed) return;  // not the first wait of this frame
  const uint64_t now = NowNs();
  if (now <= resumed) return;
  // A frame longer than a whole period (a stall, a loading screen) counts as one period: no delay until it has left
  // the window.
  g_work_ns[g_work_next] = std::min(now - resumed, g_period_ns.load(std::memory_order_relaxed));
  g_work_next = (g_work_next + 1) % kWindow;
  g_work_count = std::min(g_work_count + 1, kWindow);
  g_work_max_report_ns.store(std::max(g_work_max_report_ns.load(std::memory_order_relaxed), now - resumed),
                             std::memory_order_relaxed);
}

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
  // futari_late_input: the frame's work ends where the wait begins, also when the vblank has passed already (a frame
  // that was late: its length must count).
  NoteFrameWork();
  const uint64_t seen = g_bumps.load(std::memory_order_seq_cst);
  if (LoadCounter(base) != last) return;
  const auto before = std::chrono::steady_clock::now();
  rex::latency::TraceEvent("game thread waits for the frame counter");
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
  rex::latency::TraceEvent(woken ? "game thread woke: the counter moved" : "game thread woke: 2 ms passed");
  g_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now() - before).count()),
                 std::memory_order_relaxed);
}

}  // namespace

// The vertical-blank callback: the original increment, then wake the game thread.
REX_EXTERN(__imp__sub_8216AD38);
REX_HOOK_RAW(sub_8216AD38) {
  g_base.store(base, std::memory_order_release);
  {  // futari_late_input: the vblank's time and period
    static bool installed = false;
    if (!installed) {
      installed = true;
      // 2026-10-20: one read delay at a time. This one first: banjo-nx's (banjo_late_input, Renut's render thread's
      // and GPU queue's slack) delayed the read 0.0 ms in Futari on the PC (the game thread waits on its vblank, not in
      // Swap: no render-thread slack to see), where this one gave ~6 ms input -> swap instead of 16.7. Renut's installs
      // only with this one off (render_thread.cpp).
      if (REXCVAR_GET(futari_late_input)) {
        rex::input::SetFrameReadDelayFunction(&FrameReadDelay);
        REXLOG_INFO("[native] late input: the game reads its controllers as late as its frame allows "
                    "(futari_late_input, margin {} us, at most {} us)", REXCVAR_GET(futari_late_input_margin_us),
                    REXCVAR_GET(futari_late_input_max_us));
      }
    }
    const uint64_t now = NowNs();
    const uint64_t previous = g_vblank_ns.exchange(now, std::memory_order_acq_rel);
    if (previous && now > previous) {
      const uint64_t interval = now - previous;
      // A smoothed period, from intervals near one frame only (a missed vblank is two).
      if (interval > 12000000 && interval < 21000000) {
        const uint64_t p = g_period_ns.load(std::memory_order_relaxed);
        g_period_ns.store((p * 15 + interval) / 16, std::memory_order_relaxed);
      }
    }
  }
  __imp__sub_8216AD38(ctx, base);
  rex::latency::TraceEvent("guest vblank callback: frame counter + 1");
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
    const uint64_t delays = g_delays.exchange(0), delay_us = g_delay_sum_us.exchange(0);
    const uint64_t work_max = g_work_max_report_ns.exchange(0);
    if (delays || work_max) {
      REXLOG_INFO("[native] late input (10 s): {} frames read the controllers {:.2f} ms later on average; the "
                  "longest frame {:.2f} ms, vblank period {:.3f} ms", delays,
                  delays ? double(delay_us) / 1000.0 / double(delays) : 0.0, double(work_max) / 1e6,
                  double(g_period_ns.load(std::memory_order_relaxed)) / 1e6);
    }
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
