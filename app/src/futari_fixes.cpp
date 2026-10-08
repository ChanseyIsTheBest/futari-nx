// Mushihimesama Futari: fixes for races the Xbox 360's timing never exposed.
//
// Thread start race (first Switch boot, 2026-10-05: "Unhandled guest access violation: read of guest 0x0000004C" on
// thread F8000048, 3 s after start). sub_82191640 (a constructor) starts a thread with CreateThread-style
// sub_82088D88(..., start = sub_821911D0, param = this, flags 0), so the thread runs at once. Its routine begins with
//   lwz r11,-31352(r30)   ; the manager object, global at 0x821F8588 (stored at 0x821CE5D8 by the init code)
//   lwz r10,76(r11)       ; its member at +76, filled in by the main thread after the thread was started
//   lwz r10,76(r10)       ; -> read of 0x4C when that member is still 0
// The crash dump showed the global already set (0x4000A400) and the host load of [0x4000A44C] returning 0. On the 360
// and on the PC the main thread always gets there first; on the Switch's cores the new thread won. The routine is
// given the state it always had on the console: it waits (at most 10 s) until the global and its member at +76 are
// set, then runs unchanged.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

namespace {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

constexpr uint32_t kManagerGlobal = 0x821F8588;
constexpr uint32_t kManagerMember = 76;

}  // namespace

extern "C" REX_FUNC(__imp__sub_821911D0);
REX_HOOK_RAW(sub_821911D0) {
  const auto start = std::chrono::steady_clock::now();
  bool waited = false;
  for (;;) {
    const uint32_t manager = Load32(base, kManagerGlobal);
    if (manager && Load32(base, manager + kManagerMember)) {
      break;
    }
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
      REXLOG_WARN("[fixes] sub_821911D0: manager {:08X} still not initialized after 10 s; starting anyway", manager);
      break;
    }
    waited = true;
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  if (waited) {
    REXLOG_INFO("[fixes] sub_821911D0: waited {} us for the manager object the thread reads first",
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                    .count());
  }
  __imp__sub_821911D0(ctx, base);
}
