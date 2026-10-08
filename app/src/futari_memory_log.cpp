// Memory over time, for crashes that leave no report (Switch, 2026-10-06: the whole console went down ~1 s into
// Xbox 360 mode, which keeps four times Arcade's frame history and streams the high-resolution sprite archive; no
// rex_crash.log, no Atmosphere report). On the Switch every allocation, NVK's GPU memory included (memalign in
// nouveau_horizon_memory.c), comes from newlib's heap, so mallinfo() is the real figure; the profile's "process"
// number is only the heap reservation libnx makes at startup.
//
// futari_memory_log_s = N: one "[mem]" line every N seconds (0 = off). The log is flushed every second
// (log_flush_interval), so the last line before a hard crash is at most that old.

#include "futari_memory_log.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#if REX_PLATFORM_SWITCH
#include <malloc.h>
#include <switch.h>
extern "C" size_t RexGmCommittedBytes(void);
extern "C" size_t RexGmMappedBytes(void);
extern "C" size_t RexGmChunkCount(void);
#endif

REXCVAR_DEFINE_INT32(futari_memory_log_s, 0, "Futari",
                     "Every N seconds, log heap use (GPU memory included on the Switch) and guest memory committed; "
                     "0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace futari::memory_log {
namespace {

std::atomic<bool> g_stop{false};
std::thread g_thread;

void LogOnce() {
#if REX_PLATFORM_SWITCH
  const struct mallinfo info = mallinfo();
  uint64_t total = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  // Mapped counts every mirror view: that figure, not free memory, is what hit Horizon's mapping limit in nfsmw-nx
  // (2001-0103 at ~1.8-2.1 GB mapped; docs/platform-notes.md there). The profile's "mapping limit" reads 0/0 here.
  REXLOG_INFO("[mem] heap in use {} MB (free inside the heap {} MB, heap {} MB); guest committed {} MB, mapped {} MB "
              "with mirrors, {} chunks; process total {} MB",
              size_t(info.uordblks) >> 20, size_t(info.fordblks) >> 20, size_t(info.arena) >> 20,
              RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20, RexGmChunkCount(), total >> 20);
#endif
}

}  // namespace

void Start() {
  const int32_t seconds = REXCVAR_GET(futari_memory_log_s);
  if (seconds <= 0 || g_thread.joinable()) {
    return;
  }
  g_stop = false;
  g_thread = std::thread([seconds]() {
    while (!g_stop.load(std::memory_order_relaxed)) {
      LogOnce();
      for (int32_t i = 0; i < seconds * 10 && !g_stop.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
}

void Stop() {
  g_stop = true;
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

}  // namespace futari::memory_log
