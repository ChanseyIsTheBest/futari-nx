// futari-nx (2026-10-19): the guest's vertical blank with Renut's renderer.
//
// Renut's renderer has no GPU and so no graphics interrupt, and the SDK runs no vsync thread without its own graphics
// system. Futari's game logic counts vblanks: its vblank callback (set with D3DDevice_SetVerticalBlankCallback,
// sub_820A4F78, at device+16752; Futari's sub_8216AD38) advances the frame counter at 0x82200560, which Direct3D's
// vblank handler sub_820A4C08 calls (with the device's vblank count and time, and its queue of flips waiting for a
// vblank, empty with Renut's Swap). Direct3D's interrupt dispatcher (sub_820A7DF8) asks a GPU register first, which
// is not there: this thread calls the handler itself, on a guest thread, as the SDK's vsync thread calls the
// dispatcher: a 60 Hz timer (or Futari's display-synced vblank on the Switch, futari_vblank_display_sync), or one per
// Swap in lockstep runs (vblank_per_swap). Only for the bytes it was written for (the handler's code hash).
#include "renut_engine/futari_renut_vblank.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/platform.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cmath>
#include <mutex>
#include <thread>

#include "native/futari_native.h"
#include "renut_engine/display_sync.h"  // futari-nx: DefaultWindowDisplay
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#if REX_PLATFORM_SWITCH
#include "me/me_platform_compat.h"  // RexSwitchPinCurrentThreadToCore and the priority helper
#endif

REXCVAR_DECLARE(bool, futari_vblank_sleep_exact);   // src/me/me_native_system.cpp
REXCVAR_DECLARE(bool, futari_vblank_display_sync);  // likewise
REXCVAR_DECLARE(int32_t, futari_vblank_delay_us);   // likewise
REXCVAR_DECLARE(bool, futari_vblank_phase_auto);    // likewise
REXCVAR_DECLARE(int32_t, futari_vblank_phase_max_us);  // likewise

// futari-nx (2026-10-20): banjo-nx's display thread's placement (display_sync.cpp: priority 0x2C, pinned to core 2)
// for the vblank thread (the display-synced vblank and the timer). Created as it was, the thread had the game
// threads' priority (0x3B) and no core of its own, and Horizon does not slice between threads of one priority: a
// vblank could wait for a busy game thread sharing its core. The background core holds the pipeline prebuild worker
// and the ring cleaner (0x3B, below it).
REXCVAR_DEFINE_INT32(futari_renut_vblank_core, 2, "Futari/Renut",
                     "Renut's renderer on the Switch: the vblank thread at the render thread's priority "
                     "(0x2C), pinned to this core (-1 = as created: the default priority and core)")
    .range(-1, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace futari::renut_vblank {
namespace {

constexpr uint32_t kVblankHandler = 0x820A4C08u;  // Direct3D's vblank handler (device)
constexpr uint32_t kVblankHandlerWords = 69;
constexpr uint64_t kVblankHandlerHash = 0x49BA3A1B1EAA1B6Eull;
#if REX_PLATFORM_SWITCH
constexpr int kVblankThreadPriority = 0x2C;  // the render thread's (banjo_renut_tuning.cpp kRenderThreadPriority)
#endif

std::atomic<bool> g_running{false};
rex::system::object_ref<rex::system::XHostThread> g_thread;
std::atomic<uint64_t> g_vblanks{0};

// Direct3D's vblank handler is the one this was written for (checked once, when the title's device exists).
bool SameHandler(rex::system::KernelState* kernel) {
  static const bool same = [kernel] {
    const auto* base = kernel->memory()->TranslateVirtual<const uint8_t*>(0);
    const uint64_t found = futari::native::CodeHash(base, kVblankHandler, kVblankHandlerWords);
    if (found != kVblankHandlerHash) {
      REXLOG_WARN("[renut] vblank: Direct3D's vblank handler at {:08X} differs (hash {:016X}, written for {:016X}): "
                  "no guest vblank",
                  kVblankHandler, found, kVblankHandlerHash);
    }
    return found == kVblankHandlerHash;
  }();
  return same;
}

// One vblank: Direct3D's handler on this guest thread, when the title's device exists.
void Vblank() {
  const uint32_t device = renut::gpu::native::GlobalDevice();
  if (!device) return;
  auto* thread = rex::system::XThread::GetCurrentThread();
  auto* kernel = rex::system::kernel_state();
  if (!thread || !kernel || !kernel->function_dispatcher() || !SameHandler(kernel)) return;
  thread->SetActiveCpu(2);
  uint64_t args[] = {device};
  kernel->function_dispatcher()->ExecuteInterrupt(thread->thread_state(), kVblankHandler, args, 1);
  g_vblanks.fetch_add(1, std::memory_order_relaxed);
}

#if REX_PLATFORM_SWITCH
// futari_vblank_display_sync: the guest's vblank at each refresh of the display (vi vsync event). False when the
// event cannot be had (the timer runs then).
bool FromDisplay() {
  if (!REXCVAR_GET(futari_vblank_display_sync) || rex::kernel::xboxkrnl::VblankPerSwap()) return false;
  // futari-nx (2026-10-20, banjo-nx's console finding): the default window's display (display_sync.cpp's
  // __nx_win_init); the console refuses a second open of it while the window holds it (Ryujinx did not).
  ViDisplay own{};
  ViDisplay *display = static_cast<ViDisplay *>(banjo::display_sync::DefaultWindowDisplay());
  Event vsync{};
  if (!display) {
    if (const Result rc = viOpenDefaultDisplay(&own); R_FAILED(rc)) {
      REXLOG_WARN("[renut] vblank: cannot open the display (0x{:X}): the 60 Hz timer stays", rc);
      return false;
    }
    display = &own;
  }
  if (const Result rc = viGetDisplayVsyncEvent(display, &vsync); R_FAILED(rc)) {
    REXLOG_WARN("[renut] vblank: no vsync event from the display (0x{:X}): the 60 Hz timer stays", rc);
    if (display == &own)
      viCloseDisplay(&own);
    return false;
  }
  // futari_vblank_phase_auto (the native renderer's, src/me/me_native_system.cpp): the latest guest vblank after the
  // refresh at which the newest image is still finished for the next refresh. Renut's presents carry fences
  // (Video::PresentDone): an image not finished at its refresh is shown one later; back off 2 ms and hold 5 minutes,
  // else 0.25 ms later after 3 s of images in time.
  const bool phase_auto = REXCVAR_GET(futari_vblank_phase_auto);
  const int64_t phase_max_us = REXCVAR_GET(futari_vblank_phase_max_us);
  int64_t delay_us = phase_auto ? 0 : REXCVAR_GET(futari_vblank_delay_us);
  REXLOG_INFO("[renut] vblank: the game's vertical blank follows the display's refresh, {}",
              phase_auto ? fmt::format("its phase found by measurement, at most {} us after it", phase_max_us)
                         : fmt::format("{} us after it", delay_us));
  uint64_t last = 0;
  int64_t ceiling_us = phase_max_us;
  uint64_t ceiling_until = 0;
  uint32_t good = 0;
  uint64_t presented_at_vsync = renut::gpu::Video::PresentCount();
  uint64_t phase_misses = 0, phase_report = 0;
  while (g_running.load(std::memory_order_acquire)) {
    // 100 ms: should the display stop refreshing, the game still gets a vblank now and then.
    const bool refreshed = R_SUCCEEDED(eventWait(&vsync, 100000000ull));
    if (refreshed) {
      eventClear(&vsync);
      const uint64_t presented = renut::gpu::Video::PresentCount();
      const bool new_image = presented != presented_at_vsync;
      presented_at_vsync = presented;
      const int ready = new_image ? renut::gpu::Video::PresentDone(presented) : -1;
      rex::latency::NoteDisplayVsync(ready);
      if (phase_auto) {
        const uint64_t at = armTicksToNs(armGetSystemTick());
        if (ceiling_until && at > ceiling_until) {
          ceiling_us = phase_max_us;
          ceiling_until = 0;
        }
        if (new_image && ready == 0) {
          ++phase_misses;
          ceiling_us = std::max<int64_t>(delay_us - 2000, 0);
          ceiling_until = at + 300000000000ull;
          delay_us = ceiling_us;
          good = 0;
        } else if (new_image && ready == 1 && ++good >= 180) {
          good = 0;
          delay_us = std::min<int64_t>(delay_us + 250, ceiling_us);
        }
        if (++phase_report >= 600) {  // every 10 s
          phase_report = 0;
          REXLOG_INFO("[latency] guest vblank {:.2f} ms after the display's refresh (futari_vblank_phase_auto), "
                      "ceiling {:.2f} ms, {} images not finished at their refresh in all", double(delay_us) / 1000.0,
                      double(ceiling_us) / 1000.0, phase_misses);
        }
      }
    }
    // Never two vblanks closer than 8 ms (an event that did not clear would run the game at any speed).
    const uint64_t now = armTicksToNs(armGetSystemTick());
    if (refreshed && last && now - last < 8000000ull) continue;
    if (delay_us > 0) svcSleepThread(delay_us * 1000);
    last = now;
    Vblank();
  }
  eventClose(&vsync);
  if (display == &own)
    viCloseDisplay(&own);
  return true;
}
#endif

#if !REX_PLATFORM_SWITCH
// futari-nx (2026-10-26, Linux input latency): the Switch's display-synced vblank (FromDisplay above) for a Vulkan
// renderer elsewhere. There is no vsync event to wait for, so the refreshes are found from when the guest's images
// were shown: VK_KHR_present_wait (Video::WaitPresentShown) says when each present reached the screen, a refresh
// boundary give or take the compositor's wake-up. From those times a second thread keeps the display's period and
// phase; this thread runs the guest's vblank on that grid, every m-th refresh for a display at m x 60 Hz (a display
// that is no whole multiple of 60 Hz keeps the timer). futari_vblank_phase_auto as on the Switch: an image shown a
// refresh later than the best of the recent ones moves the vblank 2 ms earlier (and caps it there for 5 minutes);
// 3 s of images on time move it 0.25 ms later, up to futari_vblank_phase_max_us.
struct DisplayGrid {
  std::mutex mutex;
  double period_ns = 0;     // the display's refresh period
  double anchor_ns = 0;     // a refresh (steady clock)
  uint64_t samples = 0;
  uint64_t last_sample_ns = 0;
  // futari_vblank_phase_auto: refreshes from submission to display, per image; the best of the recent ones.
  int64_t best_refreshes = -1;
  uint64_t best_since_ns = 0;
  uint64_t late_images = 0;   // shown a refresh later than the best (since the last read)
  uint64_t on_time = 0;       // shown at the best (since the last read)
};

uint64_t SteadyNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The refresh of the grid nearest to t.
double NearestRefresh(const DisplayGrid& g, double t) {
  return g.anchor_ns + std::round((t - g.anchor_ns) / g.period_ns) * g.period_ns;
}

// The second thread: waits for each newest guest present to be shown and moves the grid.
void ObservePresents(DisplayGrid* grid, double nominal_ns, const std::atomic<bool>* run) {
  uint64_t done = 0;
  while (run->load(std::memory_order_acquire) && g_running.load(std::memory_order_acquire)) {
    const uint64_t newest = renut::gpu::Video::PresentCount();
    if (newest == done) {
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      continue;
    }
    uint64_t submitted = 0;
    const int shown = renut::gpu::Video::WaitPresentShown(newest, 50000000ull, &submitted);
    if (shown == 0) continue;  // not yet: wait for the same one (or a newer one) again
    done = newest;
    if (shown < 0) continue;
    const double t = double(SteadyNs());
    std::lock_guard lock(grid->mutex);
    DisplayGrid& g = *grid;
    if (!g.samples) {
      g.period_ns = nominal_ns;
      g.anchor_ns = t;
    } else {
      const double since = t - g.anchor_ns;
      const double refreshes = std::max(1.0, std::round(since / g.period_ns));
      const double expected = g.anchor_ns + refreshes * g.period_ns;
      const double error = t - expected;
      if (std::fabs(error) < 0.25 * g.period_ns) {
        // Small steps: the times carry the compositor's and this thread's wake-up jitter.
        if (refreshes <= 8) g.period_ns += 0.05 * error / refreshes;
        g.anchor_ns = expected + 0.2 * error;
      } else {
        g.anchor_ns = t;  // a jump (mode change, a long stall): start the phase again from here
      }
    }
    ++g.samples;
    g.last_sample_ns = uint64_t(t);
    // Refreshes from the image's present call to the refresh it was shown at.
    if (submitted && g.samples > 60) {
      const double first = NearestRefresh(g, double(submitted)) <= double(submitted)
                               ? NearestRefresh(g, double(submitted)) + g.period_ns
                               : NearestRefresh(g, double(submitted));
      const int64_t refreshes = std::max<int64_t>(0, int64_t(std::llround((NearestRefresh(g, t) - first) / g.period_ns)));
      const uint64_t now = uint64_t(t);
      if (g.best_refreshes < 0 || refreshes < g.best_refreshes || now - g.best_since_ns > 300000000000ull) {
        g.best_refreshes = refreshes;
        g.best_since_ns = now;
      }
      if (refreshes > g.best_refreshes)
        ++g.late_images;
      else
        ++g.on_time;
    }
  }
}

// False when the vblank cannot follow the display (no present wait, a display that is no multiple of the game's
// rate): the timer runs then.
bool FromPresents(double nominal_ns) {
  if (!REXCVAR_GET(futari_vblank_display_sync) || rex::kernel::xboxkrnl::VblankPerSwap()) return false;
  // The renderer's swapchain may not exist yet: the timer's vblanks until it does, for at most 10 s.
  const uint64_t give_up = SteadyNs() + 10000000000ull;
  uint64_t next_timer = SteadyNs() + uint64_t(nominal_ns);
  while (!renut::gpu::Video::PresentWaitOn()) {
    if (!g_running.load(std::memory_order_acquire) || SteadyNs() > give_up) return false;
    if (SteadyNs() >= next_timer) {
      Vblank();
      next_timer += uint64_t(nominal_ns);
    }
    rex::thread::Sleep(std::chrono::microseconds(500));
  }
  DisplayGrid grid;
  std::atomic<bool> observe{true};
  std::thread observer(ObservePresents, &grid, nominal_ns, &observe);
  // Learning the grid: the timer's vblanks while the first images are shown (the game presents once per vblank).
  const uint64_t learn_until = SteadyNs() + 2000000000ull;
  while (g_running.load(std::memory_order_acquire)) {
    const uint64_t now = SteadyNs();
    {
      std::lock_guard lock(grid.mutex);
      if (now > learn_until && grid.samples >= 90) break;
    }
    if (now >= next_timer) {
      Vblank();
      next_timer += uint64_t(nominal_ns);
      if (now > next_timer + 250000000ull) next_timer = now + uint64_t(nominal_ns);
    }
    rex::thread::Sleep(std::chrono::microseconds(300));
    if (now > learn_until + 8000000000ull) break;  // no images for 10 s (a long load): decide with what there is
  }
  double period = 0;
  {
    std::lock_guard lock(grid.mutex);
    period = grid.samples >= 90 ? grid.period_ns : 0;
  }
  const double ratio = period > 0 ? nominal_ns / period : 0;
  const double multiple = std::round(ratio);
  if (!g_running.load(std::memory_order_acquire) || multiple < 1 || std::fabs(ratio - multiple) > 0.01 * multiple) {
    REXLOG_INFO("[renut] vblank: the display's refresh {} (futari_vblank_display_sync): the {:.2f} Hz timer stays",
                period > 0 ? fmt::format("is {:.3f} Hz, no whole multiple of the game's", 1e9 / period)
                           : std::string("could not be measured from the presents"),
                1e9 / nominal_ns);
    observe.store(false, std::memory_order_release);
    observer.join();
    return false;
  }
  const int64_t every = int64_t(multiple);
  const bool phase_auto = REXCVAR_GET(futari_vblank_phase_auto);
  const int64_t phase_max_us = REXCVAR_GET(futari_vblank_phase_max_us);
  int64_t delay_us = phase_auto ? 0 : REXCVAR_GET(futari_vblank_delay_us);
  int64_t ceiling_us = phase_max_us;
  uint64_t ceiling_until = 0;
  uint64_t good = 0, late_total = 0, report = 0;
  REXLOG_INFO("[renut] vblank: the game's vertical blank follows the display's refresh ({:.3f} Hz, every {} "
              "refresh{}, from the presents: futari_vblank_display_sync), {}",
              1e9 / period, every, every == 1 ? "" : "es",
              phase_auto ? fmt::format("its phase found by measurement, at most {} us after it", phase_max_us)
                         : fmt::format("{} us after it", delay_us));
  double last_refresh = 0;
  while (g_running.load(std::memory_order_acquire)) {
    double target;
    uint64_t late = 0, on_time = 0;
    {
      std::lock_guard lock(grid.mutex);
      const double now = double(SteadyNs());
      double refresh = last_refresh > 0 ? NearestRefresh(grid, last_refresh + double(every) * grid.period_ns)
                                        : NearestRefresh(grid, now) + grid.period_ns;
      // Behind (a stall of this thread): the next refresh of the grid from now, keeping the step.
      while (refresh + double(delay_us) * 1000.0 < now - 0.5 * grid.period_ns) refresh += double(every) * grid.period_ns;
      if (refresh - now > 4.0 * nominal_ns) refresh = NearestRefresh(grid, now) + grid.period_ns;  // the grid jumped
      last_refresh = refresh;
      target = refresh + double(delay_us) * 1000.0;
      late = grid.late_images;
      on_time = grid.on_time;
      grid.late_images = 0;
      grid.on_time = 0;
    }
    if (phase_auto) {
      const uint64_t now = SteadyNs();
      if (ceiling_until && now > ceiling_until) {
        ceiling_us = phase_max_us;
        ceiling_until = 0;
      }
      if (late) {
        late_total += late;
        ceiling_us = std::max<int64_t>(delay_us - 2000, 0);
        ceiling_until = now + 300000000000ull;
        delay_us = ceiling_us;
        good = 0;
      } else if (on_time && (good += on_time) >= 180) {
        good = 0;
        delay_us = std::min<int64_t>(delay_us + 250, ceiling_us);
      }
      if (++report >= 600) {  // every 10 s at 60 Hz
        report = 0;
        REXLOG_INFO("[latency] guest vblank {:.2f} ms after the display's refresh (futari_vblank_phase_auto, from the "
                    "presents), ceiling {:.2f} ms, {} images shown a refresh late in all",
                    double(delay_us) / 1000.0, double(ceiling_us) / 1000.0, late_total);
      }
    }
    // Sleep to 0.5 ms before the vblank, then yield to it.
    for (;;) {
      const double left = target - double(SteadyNs());
      if (left <= 0) break;
      if (left > 700000.0)
        rex::thread::Sleep(std::chrono::microseconds(int64_t((left - 500000.0) / 1000.0)));
      else
        std::this_thread::yield();
    }
    Vblank();
  }
  observe.store(false, std::memory_order_release);
  observer.join();
  return true;
}
#endif

int Loop() {
#if REX_PLATFORM_SWITCH
  // futari_renut_vblank_core (above): for the display's refresh and the timer alike.
  const int32_t core = REXCVAR_GET(futari_renut_vblank_core);
  const bool priority_ok = core < 0 || RexSwitchSetCurrentThreadPriorityOk(kVblankThreadPriority);
  const bool core_ok = core < 0 || RexSwitchPinCurrentThreadToCore(int(core));
  REXLOG_INFO("[renut] vblank thread: {} (futari_renut_vblank_core)",
              core < 0 ? std::string("the default priority and core")
                       : fmt::format("priority 0x{:X}{}, pinned to core {}{}", kVblankThreadPriority,
                                     priority_ok ? "" : " (NOT accepted)", core, core_ok ? "" : " (NOT accepted)"));
  if (FromDisplay()) return 0;
#endif
  using Clock = std::chrono::steady_clock;
  rex::system::X_VIDEO_MODE mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  const double hz = std::max(1.0, double(float(mode.refresh_rate)));
  const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
  auto next = Clock::now() + interval;
  uint64_t last_swap = rex::kernel::xboxkrnl::GuestSwapCount();
#if !REX_PLATFORM_SWITCH
  if (FromPresents(1e9 / hz)) return 0;
  next = Clock::now() + interval;
#endif
  REXLOG_INFO("[renut] vblank: a {:.2f} Hz timer{}", hz,
              rex::kernel::xboxkrnl::VblankPerSwap() ? " (vblank_per_swap: one per Swap)" : "");
#if defined(_WIN32)
  // The PC: Windows sleeps in whole milliseconds and wakes a normal-priority sleeper late on a busy machine, which
  // delivered the vblanks in bursts (the game ran 35-40 fps while its thread slept 75 % of the time). Above the game's
  // threads, sleeping to 1.5 ms before the vblank and yielding to it.
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
  while (g_running.load(std::memory_order_acquire)) {
    if (rex::kernel::xboxkrnl::VblankPerSwap()) {  // deterministic runs (cvar vblank_per_swap, see the SDK)
      if (rex::kernel::xboxkrnl::WaitSwapForVblank(last_swap, g_running)) Vblank();
      next = Clock::now() + interval;
      continue;
    }
    const auto now = Clock::now();
    if (now - next > std::chrono::milliseconds(250)) next = now;
    while (now >= next) {
      Vblank();
      next += interval;
    }
    if (REXCVAR_GET(futari_vblank_sleep_exact)) {
      const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(next - Clock::now());
#if defined(_WIN32)
      if (wait > std::chrono::microseconds(2500))
        rex::thread::Sleep(std::min(wait - std::chrono::microseconds(1500), std::chrono::microseconds(50000)));
      else
        std::this_thread::yield();
#else
      rex::thread::Sleep(std::clamp(wait, std::chrono::microseconds(200), std::chrono::microseconds(50000)));
#endif
    } else {
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
  }
  return 0;
}

}  // namespace

void Start() {
  if (g_running.load(std::memory_order_acquire)) return;
  auto* kernel = rex::system::kernel_state();
  if (!kernel) return;
  g_running.store(true, std::memory_order_release);
  g_thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel, 128 * 1024, 0, [] { return Loop(); }));
  g_thread->set_name("Renut VBlank");
  g_thread->Create();
}

void Stop() {
  if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
  if (g_thread) {
    g_thread->Wait(0, 0, 0, nullptr);
    g_thread.reset();
  }
}

uint64_t Count() {
  return g_vblanks.load(std::memory_order_relaxed);
}

}  // namespace futari::renut_vblank
