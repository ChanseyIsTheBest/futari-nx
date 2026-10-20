// Frame latency probe (rex/frame_latency.h).
#include <rex/frame_latency.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include <fmt/format.h>

namespace rex::latency {
namespace {

uint64_t Now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// The frames in flight, by the ordinal of the Swap that ends them (1-based). Far more than can be in flight.
constexpr uint32_t kFrames = 64;
struct Frame {
  std::atomic<uint64_t> ordinal{0};
  std::atomic<uint64_t> input{0};
  std::atomic<uint64_t> swap{0};
};
Frame g_frames[kFrames];
std::atomic<uint64_t> g_swaps{0};        // the game's VdSwaps so far
std::atomic<uint64_t> g_input_frame{0};  // the frame whose first controller read is recorded
std::atomic<uint64_t> g_presented{0};    // NotePresented calls

struct Sum {
  uint64_t n = 0;
  double total = 0;
  double max = 0;
  void Add(double ms) {
    ++n;
    total += ms;
    if (ms > max) max = ms;
  }
  double Mean() const { return n ? total / double(n) : 0.0; }
};

// Once per frame at the present and once per vsync: a mutex is cheap enough there.
std::mutex g_mutex;
Sum g_input_swap, g_swap_present, g_input_present, g_present_vsync, g_input_vsync;
Sum g_input_gpu;  // banjo-nx (2026-10-20): input -> the GPU finished the frame (NoteGpuDone)
uint64_t g_superseded = 0;
uint64_t g_not_ready = 0;           // images the GPU had not finished at the first vsync after their present
uint64_t g_vblanks_without_swap = 0;
bool g_vsync_seen = false;
// The last presented frame that no display vsync has followed yet.
uint64_t g_pending = 0, g_pending_present = 0, g_pending_input = 0;
bool g_pending_not_ready = false;

double Ms(uint64_t from, uint64_t to) { return to > from ? double(to - from) / 1e6 : 0.0; }

// The trace (StartTrace): events of the next n Swaps.
constexpr uint32_t kTraceEvents = 1024;
struct TraceItem {
  uint64_t ns;
  const char* what;
  uint64_t value;
};
TraceItem g_trace[kTraceEvents];
std::atomic<uint32_t> g_trace_used{0};
std::atomic<int64_t> g_trace_swaps_left{0};  // > 0 while tracing
std::atomic<bool> g_trace_done{false};

void Trace(const char* what, uint64_t value) {
  if (g_trace_swaps_left.load(std::memory_order_relaxed) <= 0) return;
  const uint32_t i = g_trace_used.fetch_add(1, std::memory_order_relaxed);
  if (i < kTraceEvents) g_trace[i] = {Now(), what, value};
}

}  // namespace

void StartTrace(uint32_t swaps) {
  g_trace_used.store(0, std::memory_order_relaxed);
  g_trace_done.store(false, std::memory_order_relaxed);
  g_trace_swaps_left.store(int64_t(swaps), std::memory_order_release);
}

void TraceEvent(const char* what) { Trace(what, 0); }

std::string TakeTrace() {
  if (!g_trace_done.exchange(false, std::memory_order_acq_rel)) return {};
  const uint32_t n = std::min<uint32_t>(g_trace_used.load(std::memory_order_acquire), kTraceEvents);
  std::string out;
  const uint64_t first = n ? g_trace[0].ns : 0;
  for (uint32_t i = 0; i < n; ++i) {
    out += fmt::format("{:9.3f} ms  {}", double(g_trace[i].ns - first) / 1e6, g_trace[i].what);
    if (g_trace[i].value) out += fmt::format(" {}", g_trace[i].value);
    out += '\n';
  }
  return out;
}

void NoteInputRead() {
  const uint64_t frame = g_swaps.load(std::memory_order_relaxed) + 1;
  Trace("controller read for frame", frame);
  if (g_input_frame.load(std::memory_order_relaxed) == frame) return;  // not the frame's first read
  g_input_frame.store(frame, std::memory_order_relaxed);
  Frame& f = g_frames[frame % kFrames];
  f.ordinal.store(0, std::memory_order_relaxed);
  f.swap.store(0, std::memory_order_relaxed);
  f.input.store(Now(), std::memory_order_relaxed);
  f.ordinal.store(frame, std::memory_order_release);
}

void NoteGuestSwap() {
  const uint64_t frame = g_swaps.fetch_add(1, std::memory_order_acq_rel) + 1;  // GuestSwapCount: release, as VdSwap had
  Trace("guest Swap", frame);
  if (g_trace_swaps_left.load(std::memory_order_relaxed) > 0 &&
      g_trace_swaps_left.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    g_trace_done.store(true, std::memory_order_release);
  }
  Frame& f = g_frames[frame % kFrames];
  if (f.ordinal.load(std::memory_order_acquire) == frame) f.swap.store(Now(), std::memory_order_release);
}

uint64_t PresentedCount() { return g_presented.load(std::memory_order_acquire); }

uint64_t SwapCount() { return g_swaps.load(std::memory_order_acquire); }

uint64_t InputTimeOf(uint64_t swap) {
  const Frame& f = g_frames[swap % kFrames];
  if (f.ordinal.load(std::memory_order_acquire) != swap) return 0;
  return f.input.load(std::memory_order_relaxed);
}

void NotePresented(uint64_t swap) {
  const uint64_t now = Now();
  Trace("presented", swap);
  g_presented.fetch_add(1, std::memory_order_release);
  Frame& f = g_frames[swap % kFrames];
  if (f.ordinal.load(std::memory_order_acquire) != swap) return;
  const uint64_t input = f.input.load(std::memory_order_relaxed);
  const uint64_t at_swap = f.swap.load(std::memory_order_acquire);
  if (!input || !at_swap) return;
  std::lock_guard lock(g_mutex);
  g_input_swap.Add(Ms(input, at_swap));
  g_swap_present.Add(Ms(at_swap, now));
  g_input_present.Add(Ms(input, now));
  if (g_vsync_seen && g_pending) ++g_superseded;  // a newer image came before a vsync showed this one
  g_pending = swap;
  g_pending_present = now;
  g_pending_input = input;
  g_pending_not_ready = false;
}

void NoteGpuDone(uint64_t swap) {
  const uint64_t now = Now();
  const Frame& f = g_frames[swap % kFrames];
  if (!swap || f.ordinal.load(std::memory_order_acquire) != swap) return;
  const uint64_t input = f.input.load(std::memory_order_relaxed);
  if (!input) return;
  std::lock_guard lock(g_mutex);
  g_input_gpu.Add(Ms(input, now));
}

void NoteDisplayVsync(int image_ready) {
  const uint64_t now = Now();
  Trace(image_ready == 0 ? "display vsync (last image not finished yet)" : "display vsync", 0);
  std::lock_guard lock(g_mutex);
  g_vsync_seen = true;
  if (!g_pending) return;
  if (image_ready == 0) {
    // Not finished on the GPU: it is scanned out from a later vsync. Counted once; the pending image stays.
    if (!g_pending_not_ready) {
      g_pending_not_ready = true;
      ++g_not_ready;
    }
    return;
  }
  g_present_vsync.Add(Ms(g_pending_present, now));
  g_input_vsync.Add(Ms(g_pending_input, now));
  g_pending = 0;
  g_pending_not_ready = false;
}

void NoteVblankWithoutSwap() {
  Trace("guest vblank without a Swap since the previous one", 0);
  std::lock_guard lock(g_mutex);
  ++g_vblanks_without_swap;
}

std::string Report() {
  std::lock_guard lock(g_mutex);
  if (!g_input_present.n) return {};
  std::string line = fmt::format(
      "input -> swap {:.1f} ms (max {:.1f}), swap -> present {:.1f} (max {:.1f}), input -> present {:.1f} (max "
      "{:.1f}); {} frames",
      g_input_swap.Mean(), g_input_swap.max, g_swap_present.Mean(), g_swap_present.max, g_input_present.Mean(),
      g_input_present.max, g_input_present.n);
  // banjo-nx (2026-10-20): what the display can show first: the frame's GPU work done (seen within a few ms).
  if (g_input_gpu.n)
    line += fmt::format("; input -> GPU done {:.1f} ms (max {:.1f}, {} frames)", g_input_gpu.Mean(), g_input_gpu.max,
                        g_input_gpu.n);
  if (g_vsync_seen) {
    line += fmt::format(
        "; present -> the display vsync it is scanned out from {:.1f} ms (max {:.1f}), input -> that vsync {:.1f} ms "
        "(max {:.1f}); {} images not finished on the GPU at the first vsync after their present, {} followed by a "
        "newer one before any vsync",
        g_present_vsync.Mean(), g_present_vsync.max, g_input_vsync.Mean(), g_input_vsync.max, g_not_ready,
        g_superseded);
  }
  if (g_vblanks_without_swap) line += fmt::format("; {} guest vblanks without a Swap (late frames)", g_vblanks_without_swap);
  g_input_swap = g_swap_present = g_input_present = g_present_vsync = g_input_vsync = g_input_gpu = Sum{};
  g_superseded = g_not_ready = g_vblanks_without_swap = 0;
  return line;
}

}  // namespace rex::latency
