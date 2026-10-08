// futari-nx: the Direct3D census (futari_d3d_census.h). sa2-nx's sa2_d3d_api.cpp, adapted: nine modules (default.xex
// and the eight game cores each link their own Direct3D), the device known from the draw hooks only, and PM4 decoded
// only on the drawing thread.
//
// Each outermost call (no other census-hooked function below it on the thread) counts and decodes the PM4 it wrote:
// the device's write pointer (+48) before and after the call. When the call runs out of space, the command buffer's
// space function (hooked by the census build) closes the old segment, decoded up to its write pointer, and opens the
// new one. Every 10 s a report lists the functions called, their calls (outermost and nested), PM4 words per call and
// packet signatures; futari_diag_d3d_census_dump names functions whose first calls are logged word by word.

#include "futari_d3d_census.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(futari_diag_d3d_census, false, "Futari",
                    "Diagnostic: count the Direct3D functions the game calls, with the PM4 packets each call writes "
                    "(a report every 10 s; needs a census build, tools/d3d_census_build.py, for more than the draw "
                    "functions). Changes nothing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(futari_diag_d3d_census_dump, "", "Futari",
                      "Diagnostic (with futari_diag_d3d_census): function addresses (hex, separated by ',' ';' or "
                      "spaces) whose first 6 outermost calls are logged with every PM4 word they wrote")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace futari::d3d {
namespace {

struct Stats {
  uint64_t calls = 0, nested = 0, words = 0, segments = 0, other_thread = 0;
  std::map<std::string, uint64_t> packets;  // signature -> count
};
struct Key {
  const char* module;
  uint32_t address;
  bool operator<(const Key& o) const {
    const int c = std::strcmp(module, o.module);
    return c != 0 ? c < 0 : address < o.address;
  }
};

std::mutex g_mutex;
std::map<Key, Stats> g_stats;
std::unordered_map<uint32_t, uint32_t> g_dumps_left;  // address -> calls still to dump
std::atomic<uint32_t> g_device{0};
std::atomic<std::thread::id> g_draw_thread{};
std::map<uint64_t, uint64_t> g_flags;  // the device's flag bytes +10940..+10944 at a draw -> draws
auto g_last_report = std::chrono::steady_clock::now();

struct Frame {
  Key key{"", 0};
  uint32_t wptr = 0, limit = 0;  // the segment being decoded: from wptr on
  bool valid = false, dump = false;
  std::string words;
};
thread_local int t_depth = 0;
thread_local Frame t_frame;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address + (address >= 0xE0000000u ? 0x1000u : 0u), 4);
  return __builtin_bswap32(v);
}

std::string Signature(uint32_t h, const uint8_t* base, uint32_t p) {
  const uint32_t type = h >> 30;
  if (type == 0) {
    const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
    const uint32_t reg = h & 0x7FFF;
    if (reg >= 0x4000 && reg < 0x4800) return fmt::format("t0 ALU consts {:04X}..+{}", reg & ~0x3Fu, count);
    if (reg >= 0x4800 && reg < 0x4900) return fmt::format("t0 fetch consts {:04X}..+{}", reg & ~0x5u, count);
    return fmt::format("t0 {:04X}+{}{}", reg, count, (h >> 15) & 1 ? " one-reg" : "");
  }
  if (type == 1) return fmt::format("t1 {:03X},{:03X}", h & 0x7FF, (h >> 11) & 0x7FF);
  if (type == 2) return "t2 filler";
  const uint32_t opcode = (h >> 8) & 0x7F;
  if (opcode == 0x10) {  // NOP: the renderer's own markers
    if (Load32(base, p + 4) == 0x46545244u) return "t3 NOP marker FTRD";
    return fmt::format("t3 NOP ({} words)", ((h >> 16) & 0x3FFF) + 1);
  }
  return fmt::format("t3 {:02X}{} ({} words)", opcode, (h & 1) ? " predicated" : "", ((h >> 16) & 0x3FFF) + 1);
}

// The packets in [first, last] (addresses of words). Caller holds g_mutex.
void Decode(Stats& s, const uint8_t* base, uint32_t first, uint32_t last) {
  if (last < first || last - first > 0x400000u) return;
  s.words += (last - first) / 4 + 1;
  if (t_frame.dump) {
    for (uint32_t p = first; p <= last; p += 4) t_frame.words += fmt::format(" {:08X}", Load32(base, p));
    t_frame.words += " |";
  }
  for (uint32_t p = first; p <= last;) {
    const uint32_t h = Load32(base, p);
    const uint32_t type = h >> 30;
    uint32_t words = 1;
    if (type == 0) words = h ? 1 + ((h >> 16) & 0x3FFF) + 1 : 1;
    else if (type == 1) words = 3;
    else if (type == 3) words = 1 + ((h >> 16) & 0x3FFF) + 1;
    if (h) ++s.packets[Signature(h, base, p)];
    p += 4 * words;
  }
}

void Report() {
  {
    std::string f;
    for (const auto& [k, n] : g_flags) f += fmt::format(" [{:010X}] x{};", k, n);
    REXLOG_INFO("[d3d census] draw-time device flags (+10940..+10944):{}", f.empty() ? " none" : f);
    g_flags.clear();
  }
  std::vector<std::pair<uint64_t, Key>> order;
  for (const auto& [key, s] : g_stats)
    if (s.calls || s.nested) order.push_back({s.calls * 4 + s.nested, key});
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  REXLOG_INFO("[d3d census] last 10 s: {} functions called", order.size());
  for (const auto& [rank, key] : order) {
    Stats& s = g_stats[key];
    std::vector<std::pair<uint64_t, std::string>> packets;
    for (const auto& [sig, n] : s.packets) packets.push_back({n, sig});
    std::sort(packets.begin(), packets.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string p;
    for (size_t k = 0; k < packets.size() && k < 16; ++k)
      p += fmt::format(" [{}] x{};", packets[k].second, packets[k].first);
    REXLOG_INFO("[d3d census] {} sub_{:08X}: {} calls ({} nested, {} on other threads), {:.1f} PM4 words/call, {} "
                "segments; packets:{}",
                key.module, key.address, s.calls, s.nested, s.other_thread,
                s.calls ? double(s.words) / double(s.calls) : 0.0, s.segments, p);
    s = Stats{};
  }
}

void ParseDumps() {
  const std::string list = REXCVAR_GET(futari_diag_d3d_census_dump);
  size_t i = 0;
  while (i < list.size()) {
    size_t j = list.find_first_of(",; ", i);
    if (j == std::string::npos) j = list.size();
    const std::string item = list.substr(i, j - i);
    if (!item.empty()) g_dumps_left[uint32_t(std::stoul(item, nullptr, 16))] = 6;
    i = j + 1;
  }
}

bool DrawingThread() {
  const std::thread::id draw = g_draw_thread.load(std::memory_order_relaxed);
  return draw == std::thread::id{} || draw == std::this_thread::get_id();
}

}  // namespace

bool CensusOn() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(futari_diag_d3d_census);
    if (v) ParseDumps();
    return v;
  }();
  return on;
}

void NoteDraw(const uint8_t* base, uint32_t device) {
  if (!CensusOn() || !device) return;
  g_device.store(device, std::memory_order_relaxed);
  if (g_draw_thread.load(std::memory_order_relaxed) == std::thread::id{})
    g_draw_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
  uint64_t k = 0;
  for (uint32_t i = 0; i < 5; ++i) k = (k << 8) | base[device + 10940 + i];
  std::lock_guard lock(g_mutex);
  ++g_flags[k];
}

void CensusEnter(uint32_t address, const uint8_t* base, const char* module) {
  if (!CensusOn()) return;
  const Key key{module, address};
  if (t_depth++ > 0) {
    std::lock_guard lock(g_mutex);
    ++g_stats[key].nested;
    return;
  }
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  t_frame.key = key;
  t_frame.valid = device != 0 && DrawingThread();
  t_frame.words.clear();
  if (t_frame.valid) {
    t_frame.wptr = Load32(base, device + 48);
    t_frame.limit = Load32(base, device + 52);
  }
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[key];
  ++s.calls;
  if (!DrawingThread()) ++s.other_thread;
  auto dump = g_dumps_left.find(address);
  t_frame.dump = t_frame.valid && dump != g_dumps_left.end() && dump->second > 0;
  if (t_frame.dump) --dump->second;
}

void CensusLeave(const uint8_t* base) {
  if (!CensusOn()) return;
  if (--t_depth > 0) return;
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[t_frame.key];
  if (t_frame.valid && device) {
    const uint32_t wptr = Load32(base, device + 48), limit = Load32(base, device + 52);
    if (limit == t_frame.limit && wptr >= t_frame.wptr) {
      Decode(s, base, t_frame.wptr + 4, wptr);
    } else {
      ++s.segments;  // a segment switch the space function's hook did not see
    }
  }
  if (t_frame.dump)
    REXLOG_INFO("[d3d census] dump {} sub_{:08X}:{}", t_frame.key.module, t_frame.key.address,
                t_frame.words.empty() ? " (no words)" : t_frame.words);
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_report >= std::chrono::seconds(10)) {
    g_last_report = now;
    Report();
  }
}

void CensusSegmentBefore(const uint8_t* base) {
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  if (!device) return;
  std::lock_guard lock(g_mutex);
  Stats& s = g_stats[t_frame.key];
  const uint32_t wptr = Load32(base, device + 48);
  if (Load32(base, device + 52) == t_frame.limit && wptr >= t_frame.wptr) Decode(s, base, t_frame.wptr + 4, wptr);
  ++s.segments;
}

void CensusSegmentAfter(const uint8_t* base) {
  if (!CensusOn() || t_depth == 0 || !t_frame.valid) return;
  const uint32_t device = g_device.load(std::memory_order_relaxed);
  if (!device) return;
  t_frame.wptr = Load32(base, device + 48);
  t_frame.limit = Load32(base, device + 52);
}

}  // namespace futari::d3d

// The game cores' hooks (futari_native_core.cpp) reach the census through these (GetProcAddress on the PC, the final
// link on the Switch).
#if defined(_WIN32)
#define FUTARI_CENSUS_EXPORT extern "C" __declspec(dllexport)
#else
#define FUTARI_CENSUS_EXPORT extern "C" __attribute__((visibility("default")))
#endif
FUTARI_CENSUS_EXPORT bool FutariCensusOn() { return futari::d3d::CensusOn(); }
FUTARI_CENSUS_EXPORT void FutariCensusEnter(uint32_t address, const uint8_t* base, const char* module) {
  futari::d3d::CensusEnter(address, base, module);
}
FUTARI_CENSUS_EXPORT void FutariCensusLeave(const uint8_t* base) { futari::d3d::CensusLeave(base); }
FUTARI_CENSUS_EXPORT void FutariCensusSegment(const uint8_t* base, int after) {
  if (after) futari::d3d::CensusSegmentAfter(base);
  else futari::d3d::CensusSegmentBefore(base);
}
FUTARI_CENSUS_EXPORT void FutariCensusNoteDraw(const uint8_t* base, uint32_t device) {
  futari::d3d::NoteDraw(base, device);
}
