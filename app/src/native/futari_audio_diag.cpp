// Diagnostic futari_audio_diag (off): how the game's XAudio2 is paced by the render-driver callback, and what the game
// asks of XACT and XAudio2 (the crackle of Xbox 360 mode, docs/futari-notes.md). Default.xex's XAudio2 and XACT3 are
// statically linked:
//  - sub_82137390: the XAudio2 thread's loop. It waits (sub_82136138, r4 = timeout in ms, -1 = none) and runs one
//    processing pass (sub_82137218) per wake.
//  - sub_82139510 / sub_82139660: the render driver's begin / end of a pass's frame (vtable 0x8201B71C slots 5, 6;
//    r5 = 1: no data, the frame is filled with silence).
//  - sub_821396C0: the render-driver callback (XAudioRegisterRenderDriverClient, through sub_82139888): it wakes the
//    XAudio2 thread, waits for its frame and submits it.
//  - FUTARI_AUDIO_API: the methods of XACT's vtables 0x820148AC-0x820149D0 and 0x82016010-0x820160A0 and of XAudio2's
//    0x8201B55C table, and sub_8212C0F8 (the per-voice resampler of 16-bit sources: 'R' events).
// Every 10 s a "[audio-diag]" line counts passes, frames ended, callbacks, the waits that ended by their timeout and the
// time the callback spent waiting. futari_audio_diag_events_from_s / _s: every event of that stretch (seconds from the
// first callback) goes to audio_diag_events.csv beside the executable; an API call's line has the calling function (the
// indirect-call census, futari_codegen_checks.cpp) and r3-r7, f1. PC only; the Switch build keeps just the XAudio2 pass
// count that futari_sound_fade.cpp uses. Scratchpad tools of 2026-10-26: diag_steps.py, diag_voices.py.

#include "../../generated/default/futari_pch.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/xthread.h>

#include "futari_native.h"

REX_EXTERN(__imp__sub_82137218);

// XAudio2 passes so far, on both platforms whatever the diagnostic's setting (futari_sound_fade.cpp waits on it).
std::atomic<uint64_t> g_futari_xaudio2_passes{0};
uint64_t FutariXAudio2Passes() { return g_futari_xaudio2_passes.load(std::memory_order_acquire); }

#if REX_PLATFORM_SWITCH

// The Switch build has no diagnostic (no census, no event file): only the pass count.
REX_HOOK_RAW(sub_82137218) {
  __imp__sub_82137218(ctx, base);
  g_futari_xaudio2_passes.fetch_add(1, std::memory_order_release);
}

#else  // PC: the diagnostic

REXCVAR_DEFINE_BOOL(futari_audio_diag, false, "Futari",
                    "Diagnostic: '[audio-diag]' lines on the pacing of the game's XAudio2 (passes, frames, callbacks)");
REXCVAR_DEFINE_INT32(futari_audio_diag_events_from_s, 0, "Futari",
                     "Diagnostic: start of the stretch whose XAudio2 / XACT events go to audio_diag_events.csv "
                     "(seconds from the first audio callback)");
REXCVAR_DEFINE_INT32(futari_audio_diag_events_s, 0, "Futari",
                     "Diagnostic: seconds of XAudio2 / XACT events written to audio_diag_events.csv; 0 = none");

extern thread_local unsigned g_futari_indirect_caller;  // futari_codegen_checks.cpp
namespace futari::native {
void CensusWriteFromAudioDiag();  // futari_codegen_checks.cpp
}

bool FutariAudioDiagWantsCallers() { return REXCVAR_GET(futari_audio_diag); }

REX_EXTERN(__imp__sub_82137390);
REX_EXTERN(__imp__sub_82136138);
REX_EXTERN(__imp__sub_82139510);
REX_EXTERN(__imp__sub_82139660);
REX_EXTERN(__imp__sub_821396C0);

namespace {

using Clock = std::chrono::steady_clock;

int64_t NowUs() {
  static const Clock::time_point start = Clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

uint32_t ThreadTag() {
  static std::atomic<uint32_t> next{1};
  thread_local uint32_t tag = 0;
  if (!tag) {
    tag = next.fetch_add(1);
    const rex::system::XThread* thread = rex::system::XThread::GetCurrentThread();
    REXLOG_INFO("[audio-diag] thread t{}: guest thread {:08X}, start {:08X}, context {:08X}{}", tag,
                thread ? thread->thread_id() : 0, thread ? thread->creation_params()->start_address : 0,
                thread ? thread->creation_params()->start_context : 0,
                thread && thread->main_thread() ? " (main)" : "");
  }
  return tag;
}

struct Event {
  int64_t us;
  uint32_t addr, caller, a, b, c, d, e, f;
  uint16_t thread;
  // P/p pass begin/end, W/w wait begin/end, B/E frame begin/end, C/c callback begin/end, A/a API call begin/end
  char kind;
};

struct Diag {
  std::mutex mutex;
  std::vector<Event> events;
  int64_t first_callback_us = -1;
  bool written = false;
  uint64_t pass_number = 0, callback_number = 0;
  // Counters of the current 10 s window.
  uint64_t passes = 0, frames_ended = 0, frames_silent = 0, callbacks = 0, waits = 0, waits_timed_out = 0;
  uint64_t api_calls = 0;
  int64_t callback_wait_us = 0, callback_wait_max_us = 0, pass_us = 0, pass_max_us = 0;
  int64_t window_start_us = -1;
  int32_t timeout_ms = 0x7FFFFFFF;
};
Diag diag;

bool On() { return REXCVAR_GET(futari_audio_diag); }

// Under the mutex.
bool Recording(int64_t us) {
  const int32_t seconds = REXCVAR_GET(futari_audio_diag_events_s);
  if (seconds <= 0 || diag.written || diag.first_callback_us < 0) return false;
  const int64_t from = diag.first_callback_us + int64_t(REXCVAR_GET(futari_audio_diag_events_from_s)) * 1000000;
  if (us < from) return false;
  if (us < from + int64_t(seconds) * 1000000) return true;
  diag.written = true;
  const auto path = rex::filesystem::GetExecutableFolder() / "audio_diag_events.csv";
  if (FILE* f = std::fopen(path.string().c_str(), "w")) {
    std::fprintf(f, "us,kind,thread,addr,caller,a,b,c,d,e,f\n");
    for (const Event& e : diag.events) {
      std::fprintf(f, "%lld,%c,%u,%08X,%08X,%08X,%08X,%08X,%08X,%08X,%08X\n",
                   static_cast<long long>(e.us - diag.first_callback_us), e.kind, e.thread, e.addr, e.caller, e.a, e.b,
                   e.c, e.d, e.e, e.f);
    }
    std::fclose(f);
  }
  REXLOG_INFO("[audio-diag] {} events in {}", diag.events.size(), path.string());
  diag.events = std::vector<Event>();
  return false;
}

void Record(char kind, int64_t us, uint32_t addr = 0, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0,
            uint32_t e = 0, uint32_t f = 0, uint32_t caller = 0) {
  if (!Recording(us)) return;
  diag.events.push_back({us, addr, caller, a, b, c, d, e, f, uint16_t(ThreadTag()), kind});
}

// Under the mutex.
void MaybeReport(int64_t us) {
  if (diag.window_start_us < 0) diag.window_start_us = us;
  if (us - diag.window_start_us < 10000000) return;
  REXLOG_INFO("[audio-diag] last 10 s: {} passes, {} frames ended ({} silent), {} callbacks; {} waits, {} ended by "
              "their {} ms timeout; callback waited {} ms in all (longest {:.2f} ms); passes took {} ms (longest "
              "{:.2f} ms); {} XACT / XAudio2 method calls",
              diag.passes, diag.frames_ended, diag.frames_silent, diag.callbacks, diag.waits, diag.waits_timed_out,
              diag.timeout_ms, diag.callback_wait_us / 1000, diag.callback_wait_max_us / 1000.0, diag.pass_us / 1000,
              diag.pass_max_us / 1000.0, diag.api_calls);
  futari::native::CensusWriteFromAudioDiag();
  diag.passes = diag.frames_ended = diag.frames_silent = diag.callbacks = diag.waits = diag.waits_timed_out = 0;
  diag.api_calls = 0;
  diag.callback_wait_us = diag.callback_wait_max_us = diag.pass_us = diag.pass_max_us = 0;
  diag.window_start_us = us;
}

uint32_t FloatBits(double value) {
  const float f = float(value);
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return bits;
}

using Original = void (*)(PPCContext&, uint8_t*);

void LoggedApi(uint32_t address, Original original, PPCContext& ctx, uint8_t* base) {
  if (!On()) return original(ctx, base);
  const uint32_t caller = g_futari_indirect_caller;
  {
    std::lock_guard<std::mutex> lock(diag.mutex);
    ++diag.api_calls;
    Record('A', NowUs(), address, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, FloatBits(ctx.f1.f64),
           caller);
  }
  original(ctx, base);
  std::lock_guard<std::mutex> lock(diag.mutex);
  Record('a', NowUs(), address, ctx.r3.u32, 0, 0, 0, 0, FloatBits(ctx.f1.f64), caller);
}

}  // namespace

// The XAudio2 thread's loop: the engine's pacing fields, once.
REX_HOOK_RAW(sub_82137390) {
  if (On()) {
    const uint32_t engine = ctx.r3.u32;
    const auto word = [&](uint32_t offset) {
      uint32_t v;
      std::memcpy(&v, futari::native::Host(base, engine + offset), 4);
      return rex::byte_swap(v);
    };
    REXLOG_INFO("[audio-diag] XAudio2 thread loop: engine {:08X}, +100 = {}, +104 = {}, +124 = {}, +128 = {}, "
                "+148 = {}",
                engine, word(100), word(104), word(124), word(128), word(148));
  }
  return __imp__sub_82137390(ctx, base);
}

REX_HOOK_RAW(sub_82137218) {
  if (!On()) {
    __imp__sub_82137218(ctx, base);
    g_futari_xaudio2_passes.fetch_add(1, std::memory_order_release);
    return;
  }
  const int64_t begin = NowUs();
  uint64_t number;
  {
    std::lock_guard<std::mutex> lock(diag.mutex);
    number = ++diag.pass_number;
    Record('P', begin, 0, uint32_t(number), uint32_t(rex::kernel::xboxkrnl::GuestSwapCount()));
  }
  __imp__sub_82137218(ctx, base);
  g_futari_xaudio2_passes.fetch_add(1, std::memory_order_release);
  const int64_t end = NowUs();
  std::lock_guard<std::mutex> lock(diag.mutex);
  Record('p', end, 0, uint32_t(number), ctx.r3.u32);
  ++diag.passes;
  diag.pass_us += end - begin;
  diag.pass_max_us = std::max(diag.pass_max_us, end - begin);
  MaybeReport(end);
}

REX_HOOK_RAW(sub_82136138) {
  if (!On()) return __imp__sub_82136138(ctx, base);
  const int32_t timeout = ctx.r4.s32;
  const int64_t begin = NowUs();
  {
    std::lock_guard<std::mutex> lock(diag.mutex);
    Record('W', begin, 0, uint32_t(timeout));
  }
  __imp__sub_82136138(ctx, base);
  const int64_t end = NowUs();
  std::lock_guard<std::mutex> lock(diag.mutex);
  Record('w', end, 0, ctx.r3.u32, uint32_t(end - begin));
  ++diag.waits;
  diag.timeout_ms = timeout;
  // The wait returns 1 for its wake event and for the timeout alike: one that lasted its whole timeout timed out.
  if (timeout >= 0 && end - begin >= int64_t(timeout) * 1000) ++diag.waits_timed_out;
}

REX_HOOK_RAW(sub_82139510) {
  if (!On()) return __imp__sub_82139510(ctx, base);
  const uint32_t driver = ctx.r3.u32;
  __imp__sub_82139510(ctx, base);
  std::lock_guard<std::mutex> lock(diag.mutex);
  Record('B', NowUs(), 0, driver, ctx.r3.u32);
}

REX_HOOK_RAW(sub_82139660) {
  if (!On()) return __imp__sub_82139660(ctx, base);
  const uint32_t driver = ctx.r3.u32, silent = ctx.r5.u32;
  __imp__sub_82139660(ctx, base);
  std::lock_guard<std::mutex> lock(diag.mutex);
  Record('E', NowUs(), 0, driver, silent);
  ++diag.frames_ended;
  if (silent == 1) ++diag.frames_silent;
}

REX_HOOK_RAW(sub_821396C0) {
  if (!On()) return __imp__sub_821396C0(ctx, base);
  const uint32_t client = ctx.r3.u32;
  const int64_t begin = NowUs();
  uint64_t number;
  {
    std::lock_guard<std::mutex> lock(diag.mutex);
    if (diag.first_callback_us < 0) diag.first_callback_us = begin;
    number = ++diag.callback_number;
    Record('C', begin, 0, client, uint32_t(number));
  }
  __imp__sub_821396C0(ctx, base);
  const int64_t end = NowUs();
  std::lock_guard<std::mutex> lock(diag.mutex);
  Record('c', end, 0, client, uint32_t(number), uint32_t(end - begin));
  ++diag.callbacks;
  diag.callback_wait_us += end - begin;
  diag.callback_wait_max_us = std::max(diag.callback_wait_max_us, end - begin);
}

// The per-voice resampler of 16-bit sources (the xWMA effects decode to 16-bit PCM): r3 = its parameters ([0] source,
// [4] output floats, [28] output count). 'R' events: parameters, source, output, count, first and last output sample.
REX_EXTERN(__imp__sub_8212C0F8);
REX_HOOK_RAW(sub_8212C0F8) {
  if (!On()) return __imp__sub_8212C0F8(ctx, base);
  const uint32_t params = ctx.r3.u32;
  const auto word = [&](uint32_t address) {
    uint32_t v;
    std::memcpy(&v, futari::native::Host(base, address), 4);
    return rex::byte_swap(v);
  };
  const uint32_t source = word(params), output = word(params + 4), count = word(params + 28);
  __imp__sub_8212C0F8(ctx, base);
  const uint32_t first = count ? word(output) : 0, last = count ? word(output + 4 * (count - 1)) : 0;
  std::lock_guard<std::mutex> lock(diag.mutex);
  Record('R', NowUs(), 0, params, source, output, count, first, last);
}

// XACT (vtables 0x82016010, 0x82016040, 0x8201607C, 0x820160A0) and XAudio2 (0x8201B55C) methods, shared stubs left out.
#define FUTARI_AUDIO_API(ADDR)                                                 \
  REX_EXTERN(__imp__sub_##ADDR);                                               \
  REX_HOOK_RAW(sub_##ADDR) { LoggedApi(0x##ADDR, __imp__sub_##ADDR, ctx, base); }
FUTARI_AUDIO_API(821064A0) FUTARI_AUDIO_API(821063C8) FUTARI_AUDIO_API(82106428) FUTARI_AUDIO_API(82106720)
FUTARI_AUDIO_API(82106828) FUTARI_AUDIO_API(821068C8) FUTARI_AUDIO_API(82106940) FUTARI_AUDIO_API(821069A8)
FUTARI_AUDIO_API(82106A10) FUTARI_AUDIO_API(820FECB0) FUTARI_AUDIO_API(82106C98) FUTARI_AUDIO_API(82108C68)
FUTARI_AUDIO_API(82108BB8) FUTARI_AUDIO_API(82108C50) FUTARI_AUDIO_API(82108C58) FUTARI_AUDIO_API(82113A78)
FUTARI_AUDIO_API(82108C60) FUTARI_AUDIO_API(82108CB8) FUTARI_AUDIO_API(82108D38) FUTARI_AUDIO_API(82109958)
FUTARI_AUDIO_API(82108D60) FUTARI_AUDIO_API(8210B028) FUTARI_AUDIO_API(8210B030) FUTARI_AUDIO_API(8210B040)
FUTARI_AUDIO_API(8210AFF8) FUTARI_AUDIO_API(82109410) FUTARI_AUDIO_API(821094F0) FUTARI_AUDIO_API(82109598)
FUTARI_AUDIO_API(82109620) FUTARI_AUDIO_API(82108E38) FUTARI_AUDIO_API(8210B000) FUTARI_AUDIO_API(8210B038)
FUTARI_AUDIO_API(8210A078) FUTARI_AUDIO_API(8210A0E8) FUTARI_AUDIO_API(8210A998) FUTARI_AUDIO_API(8210A190)
FUTARI_AUDIO_API(82109D20) FUTARI_AUDIO_API(8210A200) FUTARI_AUDIO_API(8210B2D8) FUTARI_AUDIO_API(8210A420)
FUTARI_AUDIO_API(82109040) FUTARI_AUDIO_API(82109DB8) FUTARI_AUDIO_API(82109118) FUTARI_AUDIO_API(82109188)
FUTARI_AUDIO_API(82109220) FUTARI_AUDIO_API(82136B38) FUTARI_AUDIO_API(82137340) FUTARI_AUDIO_API(82138618)
FUTARI_AUDIO_API(82137618) FUTARI_AUDIO_API(82137620) FUTARI_AUDIO_API(82137630) FUTARI_AUDIO_API(82137628)
FUTARI_AUDIO_API(82139260) FUTARI_AUDIO_API(82137638) FUTARI_AUDIO_API(82139270) FUTARI_AUDIO_API(82139268)
FUTARI_AUDIO_API(82137450) FUTARI_AUDIO_API(821374C8) FUTARI_AUDIO_API(82137530) FUTARI_AUDIO_API(821390F0)
FUTARI_AUDIO_API(821375A0) FUTARI_AUDIO_API(82139158) FUTARI_AUDIO_API(821391D0) FUTARI_AUDIO_API(82137C20)
FUTARI_AUDIO_API(82137B38) FUTARI_AUDIO_API(82137B48) FUTARI_AUDIO_API(82137B40) FUTARI_AUDIO_API(82137B30)
FUTARI_AUDIO_API(82137B28) FUTARI_AUDIO_API(82137B20) FUTARI_AUDIO_API(82137BB0) FUTARI_AUDIO_API(82137B10)
FUTARI_AUDIO_API(82137870) FUTARI_AUDIO_API(821378E8) FUTARI_AUDIO_API(82137958) FUTARI_AUDIO_API(821379C0)
FUTARI_AUDIO_API(82137A28) FUTARI_AUDIO_API(82137D60) FUTARI_AUDIO_API(82137F28) FUTARI_AUDIO_API(82138000)
FUTARI_AUDIO_API(821380D8) FUTARI_AUDIO_API(821381A0) FUTARI_AUDIO_API(82138288) FUTARI_AUDIO_API(82137AA0)
FUTARI_AUDIO_API(82138B20) FUTARI_AUDIO_API(82138718) FUTARI_AUDIO_API(821387D0) FUTARI_AUDIO_API(82138890)
FUTARI_AUDIO_API(821384F0) FUTARI_AUDIO_API(821389A0) FUTARI_AUDIO_API(82138A60) FUTARI_AUDIO_API(82138528)
FUTARI_AUDIO_API(82139280) FUTARI_AUDIO_API(82139278) FUTARI_AUDIO_API(82139238) FUTARI_AUDIO_API(82139240)
FUTARI_AUDIO_API(82139250) FUTARI_AUDIO_API(821395B0) FUTARI_AUDIO_API(821395F8) FUTARI_AUDIO_API(821398F8)
FUTARI_AUDIO_API(82139900) FUTARI_AUDIO_API(82139A60) FUTARI_AUDIO_API(82139AE0) FUTARI_AUDIO_API(82139908)
FUTARI_AUDIO_API(82139910) FUTARI_AUDIO_API(82139920) FUTARI_AUDIO_API(82139928) FUTARI_AUDIO_API(821394B0)
FUTARI_AUDIO_API(82139BD8)
// The game's sound thread (sub_821CB108) calls the function its object holds at +572 every loop; sub_821CA5B8 sets it.
REX_EXTERN(__imp__sub_821CA5B8);
REX_HOOK_RAW(sub_821CA5B8) {
  if (On()) REXLOG_INFO("[audio-diag] sound thread object {:08X}: callback {:08X}", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_821CA5B8(ctx, base);
}

// XACT's public interfaces (vtables 0x820148AC, 0x820148F0, 0x82014958, 0x820149D0).
FUTARI_AUDIO_API(820F0020) FUTARI_AUDIO_API(820EFC38) FUTARI_AUDIO_API(820F0140)
FUTARI_AUDIO_API(820EFF18) FUTARI_AUDIO_API(820F06C8) FUTARI_AUDIO_API(820F0AB0) FUTARI_AUDIO_API(820F02C0)
FUTARI_AUDIO_API(820F0360) FUTARI_AUDIO_API(820F0530) FUTARI_AUDIO_API(820F0528) FUTARI_AUDIO_API(820EFB50)
FUTARI_AUDIO_API(820F0BC8) FUTARI_AUDIO_API(820EF8D8) FUTARI_AUDIO_API(820EF0C0) FUTARI_AUDIO_API(820EF208)
FUTARI_AUDIO_API(820EF250) FUTARI_AUDIO_API(820EEC10) FUTARI_AUDIO_API(820EF018)
FUTARI_AUDIO_API(820EF070) FUTARI_AUDIO_API(820F1198) FUTARI_AUDIO_API(820F1200)
FUTARI_AUDIO_API(820F0EF8) FUTARI_AUDIO_API(820F0CA0) FUTARI_AUDIO_API(820F0F38) FUTARI_AUDIO_API(820F0FA8)
FUTARI_AUDIO_API(820F0F90) FUTARI_AUDIO_API(820F0D10) FUTARI_AUDIO_API(820F0E38) FUTARI_AUDIO_API(820F14A8)
FUTARI_AUDIO_API(820F0E98) FUTARI_AUDIO_API(820F0DD8) FUTARI_AUDIO_API(820F0C98) FUTARI_AUDIO_API(820F15F8)
FUTARI_AUDIO_API(820F2458) FUTARI_AUDIO_API(820F2540) FUTARI_AUDIO_API(820F2598) FUTARI_AUDIO_API(820F28E0)
FUTARI_AUDIO_API(820F2698) FUTARI_AUDIO_API(820F2748) FUTARI_AUDIO_API(820F2838) FUTARI_AUDIO_API(820F23D8)
FUTARI_AUDIO_API(820F1600) FUTARI_AUDIO_API(820F17D8) FUTARI_AUDIO_API(820F1810) FUTARI_AUDIO_API(820F1850)
FUTARI_AUDIO_API(820F1860) FUTARI_AUDIO_API(820F1878) FUTARI_AUDIO_API(820F1820) FUTARI_AUDIO_API(820F1830)
FUTARI_AUDIO_API(820F1840) FUTARI_AUDIO_API(820F4B40) FUTARI_AUDIO_API(820F1650) FUTARI_AUDIO_API(820F1660)
FUTARI_AUDIO_API(820F2930) FUTARI_AUDIO_API(820F2990) FUTARI_AUDIO_API(820E9C00) FUTARI_AUDIO_API(820F2378)
FUTARI_AUDIO_API(820F1710) FUTARI_AUDIO_API(820F19F8) FUTARI_AUDIO_API(820F1F38) FUTARI_AUDIO_API(820F1F88)
FUTARI_AUDIO_API(820F1A90) FUTARI_AUDIO_API(820F1AD0) FUTARI_AUDIO_API(820F1AE0) FUTARI_AUDIO_API(820F1AF8)
FUTARI_AUDIO_API(820F1AA0) FUTARI_AUDIO_API(820F1AB0) FUTARI_AUDIO_API(820F1AC0) FUTARI_AUDIO_API(820F3690)
FUTARI_AUDIO_API(820F1B78) FUTARI_AUDIO_API(820F1B58) FUTARI_AUDIO_API(820F3D88) FUTARI_AUDIO_API(820F1DE8)
FUTARI_AUDIO_API(820F1E80) FUTARI_AUDIO_API(820F1B68) FUTARI_AUDIO_API(820F3008) FUTARI_AUDIO_API(820F37F0)
FUTARI_AUDIO_API(820F1C30) FUTARI_AUDIO_API(820F1A78) FUTARI_AUDIO_API(820F1A80) FUTARI_AUDIO_API(820F1A88)
FUTARI_AUDIO_API(820F1ED0) FUTARI_AUDIO_API(820F22D8) FUTARI_AUDIO_API(820F2AA0)

#endif  // REX_PLATFORM_SWITCH
