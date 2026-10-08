// futari-nx: the native Direct3D layer (futari_api.h), first entry point: D3DDevice_BeginVertices, which every Futari
// draw goes through (DrawVerticesUP calls it; sprites in the menus and in the game cores).
//
// Direct3D's BeginVertices began with the draw-time state flush (FlushState, inlined): the dirty shader constants, the
// shader load, the dirty register groups, the dirty fetch constants and the misc groups went out as PM4 from the
// device's shadows, then the draw's packets. Here the same work writes a host record of ops (kRun register runs; kType3
// for packets whose effect is more than registers: shader loads, literal constant loads, events, waits, the draw) into
// the direct-state ring, named by a 3-word marker NOP ['FTRA'] in fresh command buffer space, and the device is left
// exactly as Direct3D leaves it. Transliterated from Futari's own listing (default.xex sub_8209D800, the same code in the
// eight cores; tools/ppcfn.py prints it), with sa2-nx's sa2_api_internal.h as the guide: Futari's XDK (2.0.9328) has
// the same shadows and dirty masks as SA2's 2012 XDK at the same offsets up to +10900, later fields moved, and a few
// helpers differ (SET_CONSTANT instead of 0x55 in the render backend re-send, no misc bits 62-63 in the flush).
//
// The rare paths run Direct3D's own helper with the command buffer pointed at a scratch area (RedirectedCall): the
// shader load (group bits 17-20: a shader change, 2.3 % of draws in the census) and the re-send of 0x2203 / 0x2000 /
// 0x2104. Their packets become ops as they are, and their device and shader-object updates are Direct3D's own, so
// they are exact by construction. Everything else is native.
//
// Covered states: the device's flag byte +10940 is 0 at every draw of the census (no predicated tiling); any other value
// runs the original instead, checked before anything is changed (counted in the [api] line).
//
// Verify mode (futari_cpu_verify): the original runs first on a copy of the device whose command buffer is a scratch
// area; the guest regions it writes outside the device are captured and put back; then the native code runs for real
// and the two are compared: ops against the original's packets (canonical form), device bytes, regions, inline
// allocations (sa2-nx's shadow runs and their five traps, docs/api-renderer.md).
//
// ARM first (sa2-nx's round 9-11 lessons): ops written through a pointer cursor in place in the ring, register runs
// byte-swapped with NEON rev32, the thread's state reached once per call, guest helpers called on a small per-thread
// context, every 16th call timed for the "[api] drawing-thread cost" line.

#include "futari_api.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "me_native_system.h"
#include "native/futari_native.h"

REXCVAR_DEFINE_BOOL(futari_api_draws, false, "Futari",
                    "Native Direct3D layer: BeginVertices (every draw: its state flush, shader load and draw) as native "
                    "code that writes the ring's records directly, no PM4 for the draw (verify mode compares every call "
                    "with the original run on a device copy)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
REXCVAR_DEFINE_BOOL(futari_api_window_scissor, false, "Futari",
                    "Native Direct3D layer: the window offset / scissor writer (registers 0x2080-0x2082, which "
                    "SetViewport, SetScissorRect and the render target setters write straight into the command buffer) "
                    "as a native record (verify mode compares every call with the original on a device copy)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
REXCVAR_DEFINE_BOOL(futari_api_shader_load, false, "Futari",
                    "Native Direct3D layer: the draw-time shader load as native code (the pixel shader's literal "
                    "constants and load, the shader program registers, the vertex binding's load and fence; the inline "
                    "copy, the binding match and the depth helper stay Direct3D's own). Off: Direct3D's own shader load "
                    "into a scratch buffer (futari_api_draws)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
REXCVAR_DEFINE_BOOL(futari_api_clear, false, "Futari",
                    "Native Direct3D layer: D3DDevice_Clear's packets become a record (Direct3D's own code on a scratch "
                    "command buffer; no PM4 in the real one)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
REXCVAR_DEFINE_BOOL(futari_api_resolve, false, "Futari",
                    "Native Direct3D layer: D3DDevice_Resolve's packets become a record (Direct3D's own code on a "
                    "scratch command buffer; no PM4 in the real one)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);  // futari_enhancements.cpp
REXCVAR_DECLARE(bool, futari_cpu_verify);

namespace futari::api {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// PowerPC rotate-and-mask, IBM bit numbering (bit 0 = most significant).
constexpr uint32_t Mask32(int mb, int me) {
  const uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
  return mb <= me ? (a & b) : (a | b);
}
constexpr uint32_t Rlwinm(uint32_t s, int sh, int mb, int me) { return std::rotl(s, sh) & Mask32(mb, me); }
constexpr uint32_t Rlwimi(uint32_t a, uint32_t s, int sh, int mb, int me) {
  const uint32_t m = Mask32(mb, me);
  return (std::rotl(s, sh) & m) | (a & ~m);
}
constexpr uint64_t Mask64(int mb, int me) {
  const uint64_t a = ~0ull >> mb, b = ~0ull << (63 - me);
  return mb <= me ? (a & b) : (a | b);
}
constexpr uint64_t Rldicr(uint64_t s, int sh, int me) { return std::rotl(s, sh) & Mask64(0, me); }

// ---------------------------------------------------------------------------------------------------------------------
// Futari's Direct3D device (XDK 2.0.9328), the fields BeginVertices reads and writes.
namespace dev {
constexpr uint32_t kDirtyVsConsts = 0, kDirtyPsConsts = 8, kDirtyGroups = 16, kDirtyFetch = 24, kDirtyMisc = 32,
                   kDirtyRb = 40;  // groups the re-send helper handles
constexpr uint32_t kWptr = 48, kLimit = 52, kSoftLimit = 56;  // the command buffer: last word written, segment end, end - 160
constexpr uint32_t kShadowFetch = 1152, kShadowVsConsts = 1920, kShadowPsConsts = 6016, kShadowBoolLoop = 10112;
constexpr uint32_t kShadow2388 = 10272, kShadow2000 = 10368, kShadow2100 = 10444, kShadow2180 = 10528,
                   kShadow2200 = 10548, kShadow2280 = 10596, kShadow2300 = 10680, kShadow2380 = 10832;
constexpr uint32_t kReg2007 = 10396, kReg2203 = 10560;
constexpr uint32_t kFlags = 10940;  // +10940..+10944
constexpr uint32_t kVertexDecl = 11992, kBoundStride = 12008, kVizTable = 12298, kVizIndex = 12363,
                   kStreamStride = 12704, kPixelShader = 12868, kVertexShader = 12872;
constexpr uint32_t kDrawEnd = 13652, kDrawData = 13656, kDrawWords = 13664;  // EndVertices: +48 = +13652
constexpr uint32_t kReg2001 = 10372, kReg2180 = 10528, kReg2181 = 10532, kReg2182 = 10536, kReg2208 = 10580;
constexpr uint32_t kFenceNext = 10908;      // the fence the next submission carries
constexpr uint32_t kFlags42 = 10942, kFlags43 = 10943;
constexpr uint32_t kSurfaceInfo = 12616;    // the bound color target's surface object (+28: its 0x2001 value)
constexpr uint32_t kDebugObject = 13712;
constexpr uint32_t kCompare = 0x3C00;  // compared in verify mode (the fields above end below it)
}  // namespace dev

// ---------------------------------------------------------------------------------------------------------------------
// Guest memory.
inline uint8_t* HostOf(uint8_t* base, uint32_t a) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return base + a + (a >= 0xE0000000u ? 0x1000u : 0u);  // the generated code's REX_PHYS_HOST_OFFSET
#else
  return base + a;
#endif
}
struct Mem {
  uint8_t* base;
  const uint8_t* Host(uint32_t a) const { return HostOf(base, a); }
  uint8_t L8(uint32_t a) const { return *HostOf(base, a); }
  uint16_t L16(uint32_t a) const { return rex::memory::load_and_swap<uint16_t>(HostOf(base, a)); }
  uint32_t L32(uint32_t a) const { return rex::memory::load_and_swap<uint32_t>(HostOf(base, a)); }
  uint64_t L64(uint32_t a) const { return rex::memory::load_and_swap<uint64_t>(HostOf(base, a)); }
  void S8(uint32_t a, uint8_t v) { *HostOf(base, a) = v; }
  void S32(uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(HostOf(base, a), v); }
  void S64(uint32_t a, uint64_t v) { rex::memory::store_and_swap<uint64_t>(HostOf(base, a), v); }
};

// Guest words (big-endian) to host order: NEON rev32 four words an instruction (sa2-nx round 10).
inline void SwapCopy(uint32_t* __restrict dst, const uint8_t* __restrict src, uint32_t n) {
  uint32_t i = 0;
#if defined(__aarch64__)
  for (; i + 8 <= n; i += 8) {
    const uint8x16_t a = vld1q_u8(src + 4 * i), b = vld1q_u8(src + 4 * i + 16);
    vst1q_u8(reinterpret_cast<uint8_t*>(dst + i), vrev32q_u8(a));
    vst1q_u8(reinterpret_cast<uint8_t*>(dst + i + 4), vrev32q_u8(b));
  }
  if (i + 4 <= n) {
    vst1q_u8(reinterpret_cast<uint8_t*>(dst + i), vrev32q_u8(vld1q_u8(src + 4 * i)));
    i += 4;
  }
#endif
  for (; i < n; ++i) {
    uint32_t v;
    std::memcpy(&v, src + 4 * i, 4);
    dst[i] = __builtin_bswap32(v);
  }
}

// The physical address Direct3D writes in a packet for a guest address: the 0xE0000000 range is 4 KB further.
constexpr uint32_t Physical(uint32_t a) { return Rlwinm(Rlwinm(a, 12, 20, 31) + 0x200u, 0, 19, 19) + (a & 0x1FFFFFFFu); }

// ---------------------------------------------------------------------------------------------------------------------
// Ops: the direct-state record format (me_native_system.cpp, namespace direct).
enum Op : uint32_t { kRun = 1, kRunSingle = 2, kType1 = 3, kType3 = 4 };
constexpr uint32_t kOpsCapacity = 1u << 15;  // words; a call's ops (an inline vertex shader is at most 16 K words)

constexpr bool IsDrawHeader(uint32_t header) {
  const uint32_t opcode = (header >> 8) & 0x7F;
  return opcode == 0x22 || opcode == 0x36;
}

// A call's ops, written through a pointer cursor (a store of an op word cannot alias it: sa2-nx round 10, about 11
// instructions an op word with a uint32_t count on the A57).
struct Ops {
  uint32_t* buf;
  uint32_t* cur;
  uint32_t* lim;
  bool overflow = false;
  bool in_ring = false;
  uint32_t* draw = nullptr;  // the draw op's address word, set once the marker's place is known
  explicit Ops(uint32_t* b) : buf(b), cur(b), lim(b + kOpsCapacity) {}
  uint32_t size() const { return uint32_t(cur - buf); }
  bool Room(uint32_t words) {
    if (uint32_t(lim - cur) >= words) return true;
    overflow = true;
    return false;
  }
  void Run(const Mem& m, uint32_t reg, uint32_t count, uint32_t src) {
    if (!count || !Room(2 + count)) return;
    uint32_t* const p = cur;
    p[0] = kRun | (count << 8);
    p[1] = reg;
    SwapCopy(p + 2, m.Host(src), count);
    cur = p + 2 + count;
  }
  void Reg(uint32_t reg, uint32_t v) {
    if (!Room(3)) return;
    uint32_t* const p = cur;
    p[0] = kRun | (1u << 8);
    p[1] = reg;
    p[2] = v;
    cur = p + 3;
  }
  template <size_t N>
  void Regs(uint32_t reg, const uint32_t (&values)[N]) {
    if (!Room(2 + N)) return;
    uint32_t* const p = cur;
    p[0] = kRun | (uint32_t(N) << 8);
    p[1] = reg;
    for (size_t k = 0; k < N; ++k) p[2 + k] = values[k];
    cur = p + 2 + N;
  }
  // A type-3 packet as the ring runs it (Type3), payload in guest byte order, `address` naming the packet.
  template <size_t N>
  void Type3(uint32_t header, uint32_t address, const uint32_t (&words)[N]) {
    if (!Room(3 + N)) return;
    uint32_t* const p = cur;
    if (IsDrawHeader(header)) draw = p + 2;
    p[0] = kType3 | (uint32_t(N) << 8);
    p[1] = header;
    p[2] = address;
    for (size_t k = 0; k < N; ++k) p[3 + k] = __builtin_bswap32(words[k]);
    cur = p + 3 + N;
  }
};

// Packets a guest helper wrote (RedirectedCall's scratch area: [first, last] word addresses) as ops: register runs and
// type-1 pairs in host order, type-3 packets with their words (any opcode: the ring's Type3 runs it), NOPs and fillers
// dropped. False on a packet that runs past `last` (cannot happen with Direct3D's helpers; the call falls back).
bool DecodeInto(Ops& ops, uint8_t* base, uint32_t first, uint32_t last) {
  for (uint32_t p = first; p <= last;) {
    const uint8_t* const hp = HostOf(base, p);
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(hp);
    const uint32_t type = h >> 30;
    if (type == 0) {
      if (!h) {
        p += 4;
        continue;
      }
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
      if (p + 4 * count > last || !ops.Room(2 + count)) return false;
      uint32_t* const o = ops.cur;
      o[0] = (((h >> 15) & 1) ? kRunSingle : kRun) | (count << 8);
      o[1] = h & 0x7FFF;
      SwapCopy(o + 2, hp + 4, count);
      ops.cur = o + 2 + count;
      p += 4 * (1 + count);
    } else if (type == 1) {
      if (p + 8 > last || !ops.Room(4)) return false;
      uint32_t* const o = ops.cur;
      o[0] = kType1;
      o[1] = h;
      SwapCopy(o + 2, hp + 4, 2);
      ops.cur = o + 4;
      p += 12;
    } else if (type == 2) {
      p += 4;
    } else {
      const uint32_t opcode = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1;
      if (p + 4 * count > last) return false;
      if (opcode != 0x10) {  // NOP
        if (!ops.Room(3 + count)) return false;
        uint32_t* const o = ops.cur;
        if (IsDrawHeader(h)) ops.draw = o + 2;
        o[0] = kType3 | (count << 8);
        o[1] = h;
        o[2] = p & 0x1FFFFFFFu;
        std::memcpy(o + 3, hp + 4, size_t(count) * 4);  // guest byte order: Type3 reads them
        ops.cur = o + 3 + count;
      }
      p += 4 * (1 + count);
    }
  }
  return true;
}

// Why a call runs the original instead (checked before anything is changed; counted in the [api] line).
enum Fallback : uint32_t { kFbNone = 0, kFbFlags, kFbRedirect, kFbOps, kFbCount };
const char* const kFallbackNames[kFbCount] = {"none", "device flags", "helper packets", "ops overflow"};

// ---------------------------------------------------------------------------------------------------------------------
// Per-thread state, reached once per call: on the Switch every thread_local access is a call of __aarch64_read_tp.
struct InlineAlloc {
  uint32_t address, words;
};
struct ThreadData {
  uint32_t* ops = nullptr;     // the call's ops when the ring has no room in place
  bool in_original = false;    // verify mode: an original runs (nested native hooks pass through)
  uint32_t shadow_bump = 0;    // verify mode: the shadow run's inline allocations
  std::vector<InlineAlloc> native_allocs, shadow_allocs;
  uint32_t guest_depth = 0;
  std::vector<std::unique_ptr<PPCContext>> guest;
};
thread_local ThreadData* t_thread = nullptr;  // constant-initialized pointer: no TLS wrapper function
ThreadData& Thread() {
  ThreadData* t = t_thread;
  if (!t) [[unlikely]] {
    t = new ThreadData();
    t->ops = new uint32_t[kOpsCapacity];
    t_thread = t;
  }
  return *t;
}

// A guest function called from native code on a context of this thread's own: the caller's r1 / r2 / r13 and
// floating-point state copied in, the arguments set by the caller (sa2-nx's GuestCall: copying the whole 2.7 KB
// context per call cost more than many of the helpers).
class GuestCall {
 public:
  GuestCall(ThreadData& t, const PPCContext& caller) : t_(t) {
    if (t.guest_depth == t.guest.size()) t.guest.push_back(std::make_unique<PPCContext>());
    c_ = t.guest[t.guest_depth++].get();
    c_->r1 = caller.r1;
    c_->r2 = caller.r2;
    c_->r13 = caller.r13;
    c_->fpscr = caller.fpscr;
    c_->msr = caller.msr;
    c_->vscr_sat = caller.vscr_sat;
  }
  ~GuestCall() { --t_.guest_depth; }
  GuestCall(const GuestCall&) = delete;
  GuestCall& operator=(const GuestCall&) = delete;
  PPCContext& operator*() { return *c_; }
  PPCContext* operator->() { return c_; }

 private:
  ThreadData& t_;
  PPCContext* c_;
};

// Guest scratch: [0, kRedirectSize) RedirectedCall's command buffer.
constexpr uint32_t kRedirectSize = 0x40000;
// A redirect is running (one drawing thread: Direct3D is serialized): SpaceDuringRedirect answers the space function, and
// records the inline allocator's request to submit at the next space check (it lowers +52 / +56 when its heap fills).
std::atomic<bool> g_redirecting{false};
bool g_redirect_kick = false;
// The real command buffer's write pointer and limits while a redirect runs (RedirectedInlineAlloc swaps them in for the
// inline-data allocator, which may lower the limits to have the next space check submit).
uint32_t g_real_w = 0, g_real_lim = 0, g_real_soft = 0;
std::atomic<uint64_t> g_redirect_allocs{0};
std::atomic<uint64_t> g_redirect_kicks{0}, g_redirect_scratch_refs{0}, g_redirect_shader{0}, g_redirect_rb{0},
    g_native_shader{0};
// The drawing thread's counters: one writer, a plain load and store (no read-modify-write per call); only a report.
inline void Bump(std::atomic<uint64_t>& c, uint64_t n = 1) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
uint32_t g_scratch = 0;
uint32_t Scratch() {
  if (!g_scratch) g_scratch = rex::system::kernel_state()->memory()->SystemHeapAlloc(kRedirectSize, 0x1000);
  return g_scratch;
}

struct Ctx {
  const Module& mod;
  Mem m;
  Ops& ops;
  uint8_t* base;
  const PPCContext& ppc;
  uint32_t d;
  ThreadData& t;
  bool failed = false;  // a helper's packets could not be taken (DecodeInto): the call is lost (logged)
};

// After a redirect: the real command buffer's pointer and limits back; when the inline allocator asked for a submission
// at the next space check during the redirect (it set +52 = +48 + 160 and +56 = +48 on the scratch area, or the space
// function was reached), the same request is made on the real command buffer.
void RestoreLimits(Mem& m, uint32_t d, uint32_t w, uint32_t lim, uint32_t soft, uint32_t s) {
  // The inline-data allocator saw the real command buffer (RedirectedInlineAlloc) and may have lowered its limits.
  lim = std::min(lim, g_real_lim);
  soft = std::min(soft, g_real_soft);
  const bool lowered = m.L32(d + dev::kSoftLimit) != s + kRedirectSize - 0x1000 || g_redirect_kick;
  g_redirect_kick = false;
  m.S32(d + dev::kWptr, g_real_w);  // the allocator may have moved it (RedirectedInlineAlloc)
  if (lowered) {
    g_redirect_kicks.fetch_add(1, std::memory_order_relaxed);
    m.S32(d + dev::kLimit, std::min(lim, w + 160));
    m.S32(d + dev::kSoftLimit, std::min(soft, w));
  } else {
    m.S32(d + dev::kLimit, lim);
    m.S32(d + dev::kSoftLimit, soft);
  }
}

// One of Direct3D's own helpers with the command buffer pointed at the scratch area: what it writes there becomes ops,
// everything else it does (device fields, shader objects) is its own. The scratch area is far larger than any helper's
// packets, so the space function is never reached; +48 / +52 / +56 are put back afterwards. Returns r3.
uint64_t RedirectedCall(Ctx& c, GuestFn fn, uint64_t r4, uint64_t r5, uint64_t r6 = 0, uint64_t r7 = 0,
                        uint64_t r8 = 0, uint64_t r9 = 0, uint64_t r10 = 0) {
  const uint32_t s = Scratch();
  const uint32_t w = c.m.L32(c.d + dev::kWptr), lim = c.m.L32(c.d + dev::kLimit), soft = c.m.L32(c.d + dev::kSoftLimit);
  g_real_w = w;
  g_real_lim = lim;
  g_real_soft = soft;
  c.m.S32(c.d + dev::kWptr, s);
  c.m.S32(c.d + dev::kLimit, s + kRedirectSize - 0x100);
  c.m.S32(c.d + dev::kSoftLimit, s + kRedirectSize - 0x1000);
  const bool was_in_original = c.t.in_original;
  c.t.in_original = true;  // nested native hooks run the original: their packets land in the scratch area too
  const bool was_redirecting = g_redirecting.exchange(true, std::memory_order_relaxed);
  uint64_t r3;
  {
    GuestCall g(c.t, c.ppc);
    g->r3.u64 = c.d;
    g->r4.u64 = r4;
    g->r5.u64 = r5;
    g->r6.u64 = r6;
    g->r7.u64 = r7;
    g->r8.u64 = r8;
    g->r9.u64 = r9;
    g->r10.u64 = r10;
    fn(*g, c.base);
    r3 = g->r3.u64;
  }
  g_redirecting.store(was_redirecting, std::memory_order_relaxed);
  c.t.in_original = was_in_original;
  const uint32_t end = c.m.L32(c.d + dev::kWptr);
  if (end > s && !DecodeInto(c.ops, c.base, s + 4, end)) c.failed = true;
  RestoreLimits(c.m, c.d, w, lim, soft, s);
  return r3;
}

// ---------------------------------------------------------------------------------------------------------------------
// FlushState pieces (Futari's sub_8209EE08 / sub_8209EA70 / sub_8209ECB0 / sub_8209EB30 in default.xex): one register
// run per run of dirty bits (bit 63 first), `per_bit` registers a bit, from the shadow. (The fetch flush's 0x5000-0x5002
// end marker is not sent: no renderer code reads those registers; sa2-nx.)
void FlushRuns(Ctx& c, uint64_t mask, uint32_t reg, uint32_t shadow, uint32_t per_bit) {
  for (uint64_t r = mask; r;) {
    const int lz = std::countl_zero(r);
    r <<= lz;
    reg += uint32_t(lz) * per_bit;
    shadow += uint32_t(lz) * per_bit * 4;
    const int run = std::countl_one(r);
    const uint32_t count = uint32_t(run) * per_bit;
    c.ops.Run(c.m, reg, count, shadow);
    reg += count;
    shadow += 4 * count;
    r = run == 64 ? 0 : r << run;
  }
}

// Misc bits 49-54: register 0x2007, the texture cache coherency request (COHER_STATUS_HOST 0x10000, COHER_SIZE/BASE),
// a WAIT_REG_MEM on it, then 4 registers per bit from 0x2388.
void MiscFlush(Ctx& c, uint64_t mask) {
  c.ops.Reg(0x2007, c.m.L32(c.d + dev::kReg2007));
  c.ops.Reg(0x0A31, 0x00010000u);
  c.ops.Regs(0x0A2F, {0u, 0x1000u});
  c.ops.Type3(0xC0043C00u, 0, {3u, 0x0A31u, 0u, 0x80000000u, 8u});
  FlushRuns(c, mask, 0x2388, c.d + dev::kShadow2388, 4);
}

// A shader program's literal constants (sub_8209F9A8): one LOAD_ALU_CONSTANT per table entry. (Its other branch, flag
// 0x80 of +10940, copies them inline: excluded, +10940 is 0.)
void LiteralConstants(Ctx& c, uint32_t prog, uint32_t base_address) {
  Mem& m = c.m;
  const uint32_t off = m.L32(prog + 20);
  if (!off) return;
  const uint32_t table = prog + off;
  uint32_t e = table + 20;
  const uint32_t end = m.L32(table + 16) + e;
  while (e < end) {
    const uint32_t index = m.L16(e), count = m.L16(e + 2);
    if (!count) break;
    const uint32_t address = Physical(m.L32(e + 4) + base_address);
    c.ops.Type3(0xC0022F00u, 0, {address, Rlwinm(index, 2, 0, 29), count});
    e += 8;
  }
}

// The vertex shader binding `idx`'s program descriptor (vs + vs[(idx + 112) * 8]): +872 the microcode offset from the
// object's microcode base (vs + 32), +876 its size in bytes; its binding state is 416 bytes per binding from vs.
uint32_t BindingProgram(const Mem& m, uint32_t vs, uint32_t idx) { return vs + m.L32(vs + Rlwinm(idx + 112, 3, 0, 28)); }

// The shader load can be native for this draw (checked before anything is changed): Direct3D's debug assertion path
// (no debug object and a shader flagged 0x10) runs its own shader load (RedirectedCall) instead. The flag byte +10940
// is 0 (BeginVertices' own check), which excludes the 0x40 / 0x80 branches.
bool ShaderLoadCovered(const Mem& m, uint32_t d) {
  const uint32_t vs = m.L32(d + dev::kVertexShader), ps = m.L32(d + dev::kPixelShader);
  if (!vs) return true;
  if (m.L32(d + dev::kDebugObject) == 0 && ((m.L32(vs + 872) & 0x10) || (ps && (m.L32(ps + 40) & 0x10))))
    return false;
  return true;
}

// futari_api_shader_load: the draw-time shader load (default.xex sub_8209FC30, 604 instructions; sa2-nx's ShaderLoad
// with Futari's offsets and its two extras: the WAIT_UNTIL of +10943 bit 2 and the 0x2001 update of +10943 bit 8).
// Returns the updated group mask. The binding match (no PM4), the inline copy and the 0x2203 depth helper are
// Direct3D's own (the latter two write packets: RedirectedCall).
uint64_t ShaderLoadNative(Ctx& c, uint64_t r4) {
  Mem& m = c.m;
  const uint32_t d = c.d;
  uint64_t r17 = r4;
  int32_t r15 = -1;
  uint32_t r16 = 0;
  if (Rlwinm(uint32_t(r4), 0, 14, 14)) r17 = RedirectedCall(c, c.mod.depth_2203, r4, 0);
  const uint32_t vs = m.L32(d + dev::kVertexShader);
  const uint32_t decl = m.L32(d + dev::kVertexDecl);
  const uint32_t ps = m.L32(d + dev::kPixelShader);
  if (!vs) return r17;
  uint32_t r25 = vs + m.L32(vs + 896) + 872;  // binding 0's program (+872)
  uint32_t r22 = 0;
  uint32_t r27 = 0, r18 = 0, r20 = 0, r28 = 0;
  if (!ps) {
    if (m.L8(d + dev::kFlags43) & 2) {
      c.ops.Reg(0x05C8, 0x00020000u);  // WAIT_UNTIL
      m.S8(d + dev::kFlags43, uint8_t(Rlwinm(m.L8(d + dev::kFlags43), 0, 0, 29)));
    }
    if (m.L32(vs + 872) & 0x20) {
      r22 = 1;
      r25 = vs + m.L32(vs + 904) + 872;
    }
    const uint32_t mc = m.L32(d + dev::kReg2208);
    if ((mc & 7) != 5) {
      const uint8_t f43 = m.L8(d + dev::kFlags43);
      m.S32(d + dev::kReg2208, Rlwimi(mc, 5, 0, 29, 31));
      r17 |= 0x80008;
      if (f43 & 8) {
        r17 |= 1ull << 56;
        m.S32(d + dev::kReg2001, Rlwinm(m.L32(d + dev::kReg2001), 0, 16, 11));
      }
    }
    r28 = (m.L32(r25 + 20) & 0x1F) != 0 ? 1 : 0;
  } else {
    const uint32_t pp = ps + m.L32(ps + 64);
    r27 = pp + 40;
    r18 = m.L32(pp + 48);
    r16 = m.L32(pp + 52);
    r20 = r18;
    const uint32_t mrt = Rlwinm(r18, 0, 1, 3) > 0x10000000u ? 1u : 0u;
    const uint8_t b42 = m.L8(d + dev::kFlags42), b43 = m.L8(d + dev::kFlags43);
    m.S8(d + dev::kFlags42, uint8_t(Rlwimi(b42, Rlwinm(mrt, 3, 21, 28) | b42, 0, 28, 28)));
    m.S8(d + dev::kFlags43, uint8_t(Rlwimi(b43, Rlwinm(mrt, 1, 23, 30) | b43, 0, 30, 30)));
    if (Rlwinm(uint32_t(r17), 0, 11, 11)) {  // the pixel shader changed
      LiteralConstants(c, ps + 40, m.L32(ps + 24));
      const uint32_t ucode = m.L32(pp + 40) + m.L32(ps + 24);
      const uint32_t address = (Rlwinm(Rlwinm(ucode, 12, 20, 31) + 0x200u, 0, 19, 19) + Rlwinm(ucode, 0, 3, 30)) | 1u;
      c.ops.Type3(0xC0012700u, 0, {address, Rlwinm(m.L32(pp + 44), 30, 2, 31)});
      r17 |= 16384;
      m.S32(d + dev::kReg2182, m.L32(r27 + 24));
      const uint32_t mc = m.L32(d + dev::kReg2208);
      if ((mc & 7) != 4) {
        const uint8_t f43 = m.L8(d + dev::kFlags43);
        m.S32(d + dev::kReg2208, Rlwimi(mc, 1, 2, 29, 31));
        r17 |= 0x80008;
        if (f43 & 8) {
          r17 |= 1ull << 56;
          m.S32(d + dev::kReg2001, m.L32(m.L32(d + dev::kSurfaceInfo) + 28));
        }
      }
    }
    // The interpolator layout both programs use, under Direct3D's mask (initialized on its first use).
    const uint32_t g = c.mod.globals;
    uint32_t imask;
    const uint32_t flag = m.L32(g + 4);
    if (!(flag & 1)) {
      m.S32(g + 4, flag | 1);
      imask = 0x000603FFu;
      m.S32(g, imask);
    } else {
      imask = m.L32(g);
    }
    r28 = (m.L32(r27 + 20) & imask) != (m.L32(r25 + 20) & imask) ? 1 : 0;
  }
  // loc_8209FF2C (the 0x40 block of +10940 that follows is excluded)
  const uint32_t loc80_initial = m.L32(r25 + 8);
  const uint32_t r26 = m.L32(d + dev::kReg2208);
  const uint32_t r24 = m.L32(r25 + 12);
  const uint32_t r29 = r26;
  // loc_8209FFF8
  {
    const uint32_t mrt = Rlwinm(r20, 0, 1, 3) > 0x10000000u ? 1u : 0u;
    const uint8_t b43 = m.L8(d + dev::kFlags43);
    m.S8(d + dev::kFlags43, uint8_t(Rlwimi(mrt | b43, b43, 0, 24, 30)));
  }
  // The inline copy's SQ_PROGRAM_CNTL word lives in guest memory (Direct3D passes its address): a scratch word.
  const uint32_t loc80 = Scratch() + kRedirectSize - 16;
  m.S32(loc80, loc80_initial);
  const auto bind_inline = [&](uint32_t relink) {
    RedirectedCall(c, c.mod.bind_inline, relink, vs, loc80, decl, r25, r27, r22);
  };
  const auto binding_match = [&](uint32_t idx) {
    GuestCall g(c.t, c.ppc);
    g->r3.u64 = d;
    g->r4.u64 = vs;
    g->r5.u64 = decl;
    g->r6.u64 = idx;
    c.mod.binding_match(*g, c.base);
    return g->r3.u32 != 0;
  };
  const uint8_t b42 = m.L8(d + dev::kFlags42);
  if ((b42 & 0x80) || r28) {
    // loc_820A0138: an inline copy (relinked when the interpolators differ).
    if (Rlwinm(uint32_t(r17), 0, 11, 12)) {
      m.S8(d + dev::kFlags42, uint8_t(b42 & 191));
      LiteralConstants(c, vs + 872, m.L32(vs + 32));
      bind_inline(r28);
    }
  } else if (Rlwinm(uint32_t(r17), 0, 12, 12)) {  // the vertex shader changed
    LiteralConstants(c, vs + 872, m.L32(vs + 32));
    const uint8_t f = uint8_t(m.L8(d + dev::kFlags42) & 191);
    m.S8(d + dev::kFlags42, f);
    bool second = false;  // loc_820A0104: flag 0x40 of +10942 (the second binding matches too)
    if (decl == 0 || (m.L32(vs + 872) & 0x40)) {
      // loc_820A0118: the binding as it is.
      r15 = int32_t(r22);
      second = ((r29 ^ r26) & 7) && (m.L32(vs + 872) & 0x20);
    } else {
      if (binding_match(r22)) {
        r15 = int32_t(r22);
      } else {
        bind_inline(0);
      }
      second = ((r29 ^ r26) & 7) && (m.L32(vs + 872) & 0x20) && binding_match(1);
    }
    if (second) m.S8(d + dev::kFlags42, uint8_t(m.L8(d + dev::kFlags42) | 64));
  }
  // loc_820A0180
  if (Rlwinm(uint32_t(r17), 0, 11, 12)) {
    m.S32(d + dev::kReg2181, r24 | r16);
    r17 |= 0x10000;
    r17 |= 32768;
    m.S32(d + dev::kReg2180, m.L32(loc80) | r18);
  }
  // loc_820A0514 (no 0x40 block): the vertex binding's load, and its fence.
  if (r15 != -1) {
    const uint32_t prog = BindingProgram(m, vs, uint32_t(r15));
    const uint32_t ucode = m.L32(prog + 872) + m.L32(vs + 32);
    c.ops.Type3(0xC0012700u, 0, {Physical(ucode), Rlwinm(m.L32(prog + 876), 30, 2, 31)});
    m.S32(vs + uint32_t(r15) * 416 + 64, m.L32(d + dev::kFenceNext));
  }
  return r17;
}

// BeginVertices' inlined FlushState, in Futari's order (sub_8209D800 0x8209D850-0x8209DA78).
void FlushState(Ctx& c, uint64_t r28 /* fetch mask, slot 31's bit already cleared */) {
  Mem& m = c.m;
  const uint32_t d = c.d;
  const uint64_t r4 = m.L64(d + dev::kDirtyVsConsts);
  const uint64_t r27 = m.L64(d + dev::kDirtyMisc);
  uint64_t r30 = m.L64(d + dev::kDirtyGroups);
  const uint64_t r29 = m.L64(d + dev::kDirtyPsConsts);
  if (r4) {
    FlushRuns(c, r4, 0x4000, d + dev::kShadowVsConsts, 16);
    m.S64(d + dev::kDirtyVsConsts, 0);
  }
  if (r29) {
    FlushRuns(c, r29, 0x4400, d + dev::kShadowPsConsts, 16);
    m.S64(d + dev::kDirtyPsConsts, 0);
  }
  if (r30) {
    if (Rlwinm(uint32_t(r30), 0, 11, 14)) {
      static const bool native_load = REXCVAR_GET(futari_api_shader_load);
      if (native_load && ShaderLoadCovered(m, d)) {
        Bump(g_native_shader);
        r30 = ShaderLoadNative(c, r30);
      } else {
        Bump(g_redirect_shader);
        r30 = RedirectedCall(c, c.mod.shader_load, r30, 0);
      }
    }
    if (m.L64(d + dev::kDirtyRb) & r30) {
      Bump(g_redirect_rb);
      r30 = RedirectedCall(c, c.mod.rb_resend, r30, m.L32(d + dev::kReg2203));
    }
    if (r30 & 0xFFF) FlushRuns(c, Rldicr(r30, 52, 11), 0x2200, d + dev::kShadow2200, 1);
    if (Rlwinm(uint32_t(r30), 0, 15, 19)) FlushRuns(c, Rldicr(r30, 47, 4), 0x2180, d + dev::kShadow2180, 1);
    if (r30 & Rldicr(0xFFFF, 42, 21)) FlushRuns(c, Rldicr(r30, 6, 15), 0x2000, d + dev::kShadow2000, 1);
    if (r30 & (0xFFFFFFFFFFE00000ull & 0x000003FFFFFFFFFFull))
      FlushRuns(c, Rldicr(r30, 22, 20), 0x2100, d + dev::kShadow2100, 1);
    m.S64(d + dev::kDirtyGroups, 0);
  }
  if (r28) {
    if (r28 & 0xFFFFFFFFull) FlushRuns(c, Rldicr(r28, 32, 31), 0x4800, d + dev::kShadowFetch, 6);
    if (r28 & Rldicr(0x1FFFFF, 34, 29)) FlushRuns(c, Rldicr(r28, 9, 20), 0x2280, d + dev::kShadow2280, 1);
    m.S64(d + dev::kDirtyFetch, 0);
  }
  if (r27) {
    if (r27 & 0x3FFFFFFFFFull) FlushRuns(c, Rldicr(r27, 26, 37), 0x2300, d + dev::kShadow2300, 1);
    if (r27 & Rldicr(255, 38, 25)) FlushRuns(c, Rldicr(r27, 18, 7), 0x2380, d + dev::kShadow2380, 1);
    if (r27 & Rldicr(63, 49, 14)) MiscFlush(c, Rldicr(r27, 9, 5));
    if (r27 & Rldicr(1, 56, 63)) FlushRuns(c, 0xFFFFFFFFFF000000ull, 0x4900, d + dev::kShadowBoolLoop, 1);
    m.S64(d + dev::kDirtyMisc, 0);
  }
}

// The draw packet's first payload word (Direct3D's per-draw viz/initiator extension).
uint32_t VizWord(const Mem& m, uint32_t d) {
  const uint32_t b = m.L8(d + dev::kVizIndex);
  uint32_t v = m.L8(d + dev::kVizTable + b);
  v = Rlwimi(v, b != 0 ? 1u : 0u, 8, 23, 23);
  return Rlwinm(v & 0x1FFu, 0, 26, 23);
}

// ---------------------------------------------------------------------------------------------------------------------
// Statistics.
std::atomic<bool> g_failed{false};  // a native call could not finish: the layer is off for the rest of the run
std::atomic<uint64_t> g_native{0}, g_original{0}, g_fallbacks[kFbCount]{}, g_ops_words{0}, g_checked{0},
    g_differences{0}, g_no_space{0}, g_window{0};
int g_logged = 0;
std::atomic<uint64_t> g_timed_ns{0}, g_timed_calls{0}, g_calls{0};
uint32_t g_time_tick = 0;

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}
template <class F>
inline bool Timed(F&& f) {
  if ((++g_time_tick & 15) != 0) [[likely]] {
    const bool done = f();
    if (done) Bump(g_calls);
    return done;
  }
  const uint64_t t0 = NowNs();
  const bool done = f();
  if (done) {
    Bump(g_timed_ns, NowNs() - t0);
    Bump(g_timed_calls);
    Bump(g_calls);
  }
  return done;
}
void LogDifference(const std::string& what, const std::string& detail) {
  Bump(g_differences);
  if (g_logged++ < 16) REXLOG_ERROR("[api] DIFFERENCE: {}: {}", what, detail);
}

// ---------------------------------------------------------------------------------------------------------------------
// Verify mode: canonical forms (register values between effectful type-3 packets; fillers, NOPs, INVALIDATE_STATE and
// registers 0x5000+ dropped) and the shadow runs.
struct Item {
  uint32_t kind;  // 0 = registers, 1 = type-3 packet
  std::map<uint32_t, uint32_t> regs;
  std::vector<uint32_t> words;
  bool operator==(const Item& o) const { return kind == o.kind && regs == o.regs && words == o.words; }
};
using Canon = std::vector<Item>;
void AddReg(Canon& c, uint32_t reg, uint32_t v) {
  if (reg >= 0x5000) return;
  if (c.empty() || c.back().kind != 0) c.push_back({0, {}, {}});
  c.back().regs[reg] = v;
}
void AddType3(Canon& c, uint32_t header, const uint32_t* payload_host, uint32_t count) {
  const uint32_t opcode = (header >> 8) & 0x7F;
  if (opcode == 0x10 || opcode == 0x3B) return;
  Item it{1, {}, {}};
  it.words.push_back(header);
  it.words.insert(it.words.end(), payload_host, payload_host + count);
  c.push_back(std::move(it));
}
void CanonPm4(Canon& c, uint8_t* base, uint32_t first, uint32_t last) {
  std::vector<uint32_t> payload;
  for (uint32_t p = first; p <= last;) {
    const uint32_t h = rex::memory::load_and_swap<uint32_t>(HostOf(base, p));
    const uint32_t type = h >> 30;
    if (type == 2 || h == 0) {
      p += 4;
      continue;
    }
    if (type == 0) {
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1, reg = h & 0x7FFF;
      const bool single = (h >> 15) & 1;
      for (uint32_t i = 0; i < count; ++i)
        AddReg(c, single ? reg : reg + i, rex::memory::load_and_swap<uint32_t>(HostOf(base, p + 4 + 4 * i)));
      p += 4 * (1 + count);
    } else if (type == 1) {
      AddReg(c, h & 0x7FF, rex::memory::load_and_swap<uint32_t>(HostOf(base, p + 4)));
      AddReg(c, (h >> 11) & 0x7FF, rex::memory::load_and_swap<uint32_t>(HostOf(base, p + 8)));
      p += 12;
    } else {
      const uint32_t count = ((h >> 16) & 0x3FFF) + 1;
      payload.resize(count);
      for (uint32_t i = 0; i < count; ++i)
        payload[i] = rex::memory::load_and_swap<uint32_t>(HostOf(base, p + 4 + 4 * i));
      AddType3(c, h, payload.data(), count);
      p += 4 * (1 + count);
    }
  }
}
void CanonOps(Canon& c, const uint32_t* op, uint32_t n) {
  std::vector<uint32_t> payload;
  for (uint32_t i = 0; i < n;) {
    const uint32_t tag = op[i], count = tag >> 8;
    switch (tag & 0xFF) {
      case kRun:
        for (uint32_t k = 0; k < count; ++k) AddReg(c, op[i + 1] + k, op[i + 2 + k]);
        i += 2 + count;
        break;
      case kRunSingle:
        for (uint32_t k = 0; k < count; ++k) AddReg(c, op[i + 1], op[i + 2 + k]);
        i += 2 + count;
        break;
      case kType1:
        AddReg(c, op[i + 1] & 0x7FF, op[i + 2]);
        AddReg(c, (op[i + 1] >> 11) & 0x7FF, op[i + 3]);
        i += 4;
        break;
      case kType3:
        payload.resize(count);
        for (uint32_t k = 0; k < count; ++k) payload[k] = __builtin_bswap32(op[i + 3 + k]);
        AddType3(c, op[i + 1], payload.data(), count);
        i += 3 + count;
        break;
      default:
        i = n;
        break;
    }
  }
}
std::string Describe(const Canon& c) {
  std::string s;
  for (const Item& it : c) {
    if (s.size() > 3000) {
      s += " ...";
      break;
    }
    if (it.kind == 0) {
      s += " {";
      for (const auto& [r, v] : it.regs) s += fmt::format(" {:04X}={:08X}", r, v);
      s += " }";
    } else {
      s += " [";
      for (size_t k = 0; k < it.words.size() && k < 8; ++k) s += fmt::format(" {:08X}", it.words[k]);
      if (it.words.size() > 8) s += fmt::format(" ...{} words", it.words.size());
      s += " ]";
    }
  }
  return s;
}

constexpr uint32_t kShadowCopy = 0x6000, kShadowBuffer = 0x80000;
uint32_t g_shadow = 0;
std::atomic<bool> g_shadow_running{false};  // ShadowInlineAlloc's fast check (one global load outside verify mode)

struct Region {
  uint32_t a, n;
  std::vector<uint8_t> before, original;
};

// Guest memory outside the device the original may write: the bound vertex shader's object (binding states) and both
// bindings' program descriptors and microcode, the declaration's id, Direct3D's globals (interpolator mask, its flag, the
// declaration ids).
std::vector<Region> ExternalRegions(const Module& mod, uint8_t* base, uint32_t d) {
  std::vector<Region> r;
  auto add = [&](uint32_t a, uint32_t n) {
    if (a >= 0x40000000u && n && n <= 0x10000u) r.push_back({a, n, {}, {}});
  };
  const Mem m{base};
  const uint32_t vs = m.L32(d + dev::kVertexShader), decl = m.L32(d + dev::kVertexDecl);
  if (vs) {
    add(vs, 0x400);
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t prog = vs + m.L32(vs + Rlwinm(i + 112, 3, 0, 28));
      add(prog + 872, 8);
      add(m.L32(prog + 872) + m.L32(vs + 32), m.L32(prog + 876));
    }
  }
  if (decl) add(decl + 48, 4);
  if (mod.globals) r.push_back({mod.globals, 12, {}, {}});
  return r;
}
void Snapshot(uint8_t* base, std::vector<Region>& r, bool original) {
  for (Region& x : r) {
    auto& v = original ? x.original : x.before;
    v.resize(x.n);
    std::memcpy(v.data(), HostOf(base, x.a), x.n);
  }
}

// The bits of device byte i verify mode does not compare (0 = all compared): fields the command buffer transport and
// the inline-data allocator write, which only the native side runs for real (the shadow run's allocator answers from a
// scratch area):
//   +48..+59        the write pointer and segment limits (transport);
//   +13652..+13659  the draw's end and vertex address (the marker's end; the scratch block the shadow got);
//   +15124..+15143  the allocator's bookkeeping (sub_820A7F08 / sub_820A8138: heap position, wraps, bytes used);
//   +10942 bit 0x04 the allocator's "heap wrapped" flag (set at every draw of the census already);
//   +10992..+10999  written by the ring thread, not by Direct3D's draw path: the CP scratch registers' memory
//                   writeback (Swap writes SCRATCH_REG 0x0578 / 0x057C; SetRingBufferParameters reads +10992). The
//                   first PC verify run (2026-10-07) found +10995 flipping 01 <-> 02 between the copy and the
//                   comparison in 94 of 661,823 calls: a race with the ring, not a difference (sa2-nx's trap 2).
//   +10904..+10907  written by the ring thread too (sa2-nx's +11032, "the GPU's memory writes"): the second PC verify
//                   run found it 30 -> 00 once in 699,410 calls, between the copy and the comparison.
//   +15168..+15171  no guest code stores to it (the generated code has no store to device+15168): written by the ring
//                   thread's memory writes; the full verify run of 2026-10-07 found it 00 -> FF once in 783,874 calls.
// Others are added as verify mode names them, each with its reason.
uint8_t IgnoredBits(uint32_t i) {
  if ((i >= dev::kWptr && i < dev::kSoftLimit + 4) || (i >= dev::kDrawEnd && i < dev::kDrawData + 4) ||
      (i >= 15124 && i < 15144) || (i >= 10992 && i < 11000) || (i >= 10904 && i < 10908) ||
      (i >= 15168 && i < 15172))
    return 0xFF;
  if (i == 10942) return 0x04;
  return 0;
}

struct Shadow {
  uint32_t copy = 0;
  uint32_t first = 0;
  std::vector<Region> regions;
  bool ok = false;
};

Shadow RunShadow(ThreadData& t, const Module& mod, uint8_t* base, const PPCContext& ctx, uint32_t d,
                 GuestFn original) {
  Shadow s;
  if (!g_shadow) g_shadow = rex::system::kernel_state()->memory()->SystemHeapAlloc(kShadowCopy + kShadowBuffer, 0x1000);
  if (!g_shadow) return s;
  s.copy = g_shadow;
  const uint32_t buffer = g_shadow + kShadowCopy;
  s.regions = ExternalRegions(mod, base, d);
  Snapshot(base, s.regions, false);
  std::memcpy(HostOf(base, s.copy), HostOf(base, d), kShadowCopy);
  rex::memory::store_and_swap<uint32_t>(HostOf(base, s.copy + dev::kWptr), buffer);
  rex::memory::store_and_swap<uint32_t>(HostOf(base, s.copy + dev::kLimit), buffer + kShadowBuffer / 2 - 0x100);
  rex::memory::store_and_swap<uint32_t>(HostOf(base, s.copy + dev::kSoftLimit), buffer + kShadowBuffer / 4);
  s.first = buffer + 4;
  t.shadow_allocs.clear();
  t.native_allocs.clear();
  t.shadow_bump = buffer + kShadowBuffer / 2;
  PPCContext c = ctx;  // verify mode only: the original gets the caller's whole context, as it would
  c.r3.u64 = s.copy;
  t.in_original = true;
  g_shadow_running.store(true, std::memory_order_relaxed);
  original(c, base);
  g_shadow_running.store(false, std::memory_order_relaxed);
  t.in_original = false;
  Snapshot(base, s.regions, true);
  for (Region& x : s.regions) std::memcpy(HostOf(base, x.a), x.before.data(), x.n);
  s.ok = true;
  Bump(g_original);
  return s;
}

void CompareShadow(ThreadData& t, const char* what, uint8_t* base, uint32_t d, const Shadow& s, const Ops& ops,
                   uint32_t last) {
  Bump(g_checked);
  if (ops.overflow) LogDifference(what, "native ops overflow");
  std::vector<uint32_t> mapped(ops.buf, ops.buf + ops.size());
  if (t.native_allocs.size() != t.shadow_allocs.size())
    LogDifference(what, fmt::format("{} inline allocations, the original {}", t.native_allocs.size(),
                                    t.shadow_allocs.size()));
  for (size_t k = 0; k < std::min(t.native_allocs.size(), t.shadow_allocs.size()); ++k) {
    const InlineAlloc& a = t.native_allocs[k];
    const InlineAlloc& b = t.shadow_allocs[k];
    if (a.words != b.words) {
      LogDifference(what, fmt::format("inline allocation {}: {} words, the original {}", k, a.words, b.words));
      continue;
    }
    // The native ops' references to it (the vertex fetch constant's address word, Physical(a) | 3) stand for the
    // shadow's.
    const uint32_t from = Physical(a.address) | 3u, to = Physical(b.address) | 3u;
    for (uint32_t i = 0; i < mapped.size();) {
      const uint32_t tag = mapped[i], count = tag >> 8;
      if ((tag & 0xFF) == kRun || (tag & 0xFF) == kRunSingle) {
        for (uint32_t j = 0; j < count; ++j)
          if (mapped[i + 2 + j] == from) mapped[i + 2 + j] = to;
        i += 2 + count;
      } else if ((tag & 0xFF) == kType3) {
        i += 3 + count;
      } else if ((tag & 0xFF) == kType1) {
        i += 4;
      } else {
        break;
      }
    }
  }
  t.native_allocs.clear();
  t.shadow_allocs.clear();
  Canon want, got;
  CanonPm4(want, base, s.first, last);
  CanonOps(got, mapped.data(), uint32_t(mapped.size()));
  if (!(want == got)) LogDifference(what, fmt::format("original:{} | native:{}", Describe(want), Describe(got)));
  for (uint32_t i = 0; i < dev::kCompare; ++i) {
    const uint8_t keep = uint8_t(~IgnoredBits(i));
    const uint8_t want_b = *HostOf(base, s.copy + i), got_b = *HostOf(base, d + i);
    if ((want_b ^ got_b) & keep) {
      LogDifference(what, fmt::format("device+{} native {:02X} original {:02X}", i, got_b, want_b));
      break;
    }
  }
  for (const Region& x : s.regions) {
    if (std::memcmp(HostOf(base, x.a), x.original.data(), x.n) != 0) {
      uint32_t k = 0;
      while (k < x.n && *HostOf(base, x.a + k) == x.original[k]) ++k;
      LogDifference(what, fmt::format("guest byte {:08X} native {:02X} original {:02X}", x.a + k,
                                      *HostOf(base, x.a + k), x.original[k]));
      break;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// The record's transport while the rest of Direct3D still writes PM4: the marker NOP in the command buffer.
uint32_t MarkerSpace(Ctx& c) {
  uint32_t w = c.m.L32(c.d + dev::kWptr);
  if (w > c.m.L32(c.d + dev::kSoftLimit)) [[unlikely]] {
    GuestCall g(c.t, c.ppc);
    g->r3.u64 = c.d;
    c.mod.space(*g, c.base);
    w = g->r3.u32;
  }
  return w;
}
uint32_t PutMarker(uint8_t* base, uint32_t w, uint32_t seq) {
  uint8_t* p = HostOf(base, w + 4);
  rex::memory::store_and_swap<uint32_t>(p, 0xC0011000u);      // NOP, 2 payload words
  rex::memory::store_and_swap<uint32_t>(p + 4, 0x46545241u);  // 'FTRA'
  rex::memory::store_and_swap<uint32_t>(p + 8, seq);
  return w + 12;
}
uint32_t AllocInline(Ctx& c, uint32_t words) {
  GuestCall g(c.t, c.ppc);
  g->r3.u64 = c.d;
  g->r4.u64 = words;
  g->r5.u64 = 16;
  c.mod.alloc_inline(*g, c.base);
  const uint32_t a = g->r3.u32;
  static const bool verify = REXCVAR_GET(futari_cpu_verify);
  if (a && verify) c.t.native_allocs.push_back({a, words});
  return a;
}

Ops OpenOps(ThreadData& t) {
  if (uint32_t* const p = me::native::ReserveApiRecord(kOpsCapacity)) [[likely]] {
    Ops o{p};
    o.in_ring = true;
    return o;
  }
  return Ops{t.ops};
}
bool Publish(uint32_t& seq, const Ops& ops) {
  if (ops.overflow) [[unlikely]] {
    REXLOG_ERROR("[api] futari_api_draws: a call's ops did not fit ({} words); the native layer is off", kOpsCapacity);
    g_failed.store(true, std::memory_order_relaxed);
    return false;
  }
  seq = ops.in_ring ? me::native::CommitApiRecord(ops.buf, ops.size()) : me::native::PublishApiRecord(ops.buf, ops.size());
  if (!seq) [[unlikely]] {
    REXLOG_ERROR("[api] futari_api_draws: {}; the native layer is off",
                 ops.in_ring ? "the record's reserved space was taken by another record" : "no room in the ring of records");
    g_failed.store(true, std::memory_order_relaxed);
    return false;
  }
  Bump(g_native);
  Bump(g_ops_words, ops.size());
  return true;
}

// The native BeginVertices (sub_8209D800): the stride state, FlushState, the marker's space, the inline vertices
// (Direct3D's allocator), the draw, the record and the marker; +13652 = the marker's end (EndVertices moves +48 there).
bool BeginVerticesNative(const Module& mod, PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(futari_api_draws) || g_failed.load(std::memory_order_relaxed) || !me::native::NativeActive())
    return false;
  ThreadData& t = Thread();
  if (t.in_original) return false;  // verify mode: inside an original's shadow run
  const uint32_t d = ctx.r3.u32, type = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  if (d < 0x40000000u || d >= 0x7F000000u || !Scratch()) return false;
  Mem m{base};
  if (m.L8(d + dev::kFlags) != 0) [[unlikely]] {  // predicated tiling (bit 0) and the rest: never in the census
    Bump(g_fallbacks[kFbFlags]);
    return false;
  }
  static const bool verify = REXCVAR_GET(futari_cpu_verify);
  Shadow shadow;
  if (verify) [[unlikely]] shadow = RunShadow(t, mod, base, ctx, d, mod.begin_vertices);
  Ops ops = OpenOps(t);
  Ctx c{mod, m, ops, base, ctx, d, t};
  // The stride state: the stream-0 stride byte is the call's while FlushState runs (the vertex shader is reloaded when
  // its binding was patched for another stride); fetch slot 31's bit is left out of the flush (the draw sets it).
  const uint32_t words = Rlwinm(count * stride, 30, 2, 31);
  const uint8_t bound = m.L8(d + dev::kBoundStride);
  const uint8_t saved = m.L8(d + dev::kStreamStride);
  const uint8_t s4 = uint8_t(Rlwinm(stride, 30, 24, 31));
  m.S8(d + dev::kStreamStride, s4);
  if (s4 != bound && m.L32(d + dev::kVertexShader) != 0)
    m.S64(d + dev::kDirtyGroups, m.L64(d + dev::kDirtyGroups) | 0x80000ull);
  const uint64_t fetch = Rldicr(m.L64(d + dev::kDirtyFetch), 0, 62);
  m.S64(d + dev::kDirtyFetch, fetch);
  FlushState(c, fetch);
  if (c.failed) [[unlikely]] {
    // A helper's packets could not be taken (cannot happen with Direct3D's helpers): the device is already updated, so
    // the call cannot go back to the original. Logged once; the layer stops.
    REXLOG_ERROR("[api] futari_api_draws: a Direct3D helper's packets could not become ops; the native layer is off");
    Bump(g_fallbacks[kFbRedirect]);
    g_failed.store(true, std::memory_order_relaxed);
  }
  m.S8(d + dev::kStreamStride, saved);
  const uint32_t w = MarkerSpace(c);
  const uint32_t data = AllocInline(c, words);
  if (data) {
    c.ops.Reg(0x2102, 0);
    c.ops.Regs(0x48BA, {1u, 0u, 1u, 0u, Physical(data) | 3u, Rlwimi(0x10000002u, words, 2, 6, 29)});
    c.ops.Type3(0xC0012201u, w + 16, {VizWord(m, d), Rlwinm(count, 16, 0, 15) | (type & 0x3F) | 128});
  } else {
    Bump(g_no_space);
  }
  uint32_t seq = 0;
  if (!Publish(seq, ops)) {
    ctx.r3.u64 = 0;
    return true;  // the device is already updated: this draw is lost (logged)
  }
  const uint32_t marker_end = PutMarker(base, w, seq);
  if (data) {
    m.S64(d + dev::kDirtyFetch, m.L64(d + dev::kDirtyFetch) | 1);
    m.S32(d + dev::kDrawEnd, marker_end);
    m.S32(d + dev::kDrawData, data);
    m.S32(d + dev::kDrawWords, words);
  } else {
    m.S32(d + dev::kWptr, marker_end);  // no inline space: the state goes out, no draw
  }
  if (shadow.ok)
    CompareShadow(t, "BeginVertices", base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + (data ? dev::kDrawEnd : dev::kWptr))));
  ctx.r3.u64 = data;
  return true;
}

// The window offset / scissor writer (default.xex sub_82096B28; device, x0, y0, x1, y1): WINDOW_OFFSET 0 and the window
// scissor from the rectangle, which Direct3D writes straight into the command buffer (no shadow). Flags 0x10 / 0x20 of
// +10940 (tiling: the rectangle per tile, bin masks around it) run the original: never in the census.
bool WindowScissorNative(const Module& mod, PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(futari_api_window_scissor) || g_failed.load(std::memory_order_relaxed) ||
      !me::native::NativeActive())
    return false;
  ThreadData& t = Thread();
  if (t.in_original) return false;
  const uint32_t d = ctx.r3.u32;
  if (d < 0x40000000u || d >= 0x7F000000u || !Scratch()) return false;
  const uint32_t x0 = ctx.r4.u32, y0 = ctx.r5.u32, x1 = ctx.r6.u32, y1 = ctx.r7.u32;
  Mem m{base};
  if (m.L8(d + dev::kFlags) & 0x30) [[unlikely]] {
    Bump(g_fallbacks[kFbFlags]);
    return false;
  }
  static const bool verify = REXCVAR_GET(futari_cpu_verify);
  Shadow shadow;
  if (verify) [[unlikely]] shadow = RunShadow(t, mod, base, ctx, d, mod.window_scissor);
  Ops ops = OpenOps(t);
  Ctx c{mod, m, ops, base, ctx, d, t};
  ops.Regs(0x2080, {0u, Rlwinm(y0, 16, 2, 15) | Rlwinm(x0, 0, 18, 31), Rlwinm(y1, 16, 2, 15) | Rlwinm(x1, 0, 18, 31)});
  const uint32_t w = MarkerSpace(c);
  uint32_t seq = 0;
  if (!Publish(seq, ops)) return true;  // logged; the layer stops
  m.S32(d + dev::kWptr, PutMarker(base, w, seq));
  Bump(g_window);
  if (shadow.ok)
    CompareShadow(t, "WindowScissor", base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + dev::kWptr)));
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// A whole Direct3D entry point through the records (futari_api_clear / futari_api_resolve): its own code runs with the
// command buffer pointed at the scratch area (nested native hooks pass through, the space function does not submit), and
// what it wrote becomes the record, named by a marker in the real command buffer. Exact by construction: the device,
// the shader objects and the inline-data heap are updated by Direct3D itself. A packet that pointed into the scratch
// area (data placed in the command buffer itself) could not survive the move: none is expected (checked on every call,
// counted, and the layer stops if one appears).
// Only words that hold memory addresses are looked at: the fetch constants (0x4800-0x48FF), the resolve's copy
// destination (0x2318), and type-3 payloads other than inline microcode (IM_LOAD_IMMEDIATE), register values
// (SET_CONSTANT) and NOPs. (The first PC run flagged a word of the clear shader's microcode: 0x30052003.)
bool ScratchReference(const Ops& ops, uint32_t s, std::string* what) {
  const uint32_t lo = s, hi = s + kRedirectSize, plo = Physical(s), phi = Physical(s) + kRedirectSize;
  const auto in = [&](uint32_t v) { v &= ~3u; return (v >= lo && v < hi) || (v >= plo && v < phi); };
  for (const uint32_t* p = ops.buf; p < ops.cur;) {
    const uint32_t tag = *p, count = tag >> 8;
    switch (tag & 0xFF) {
      case kRun:
      case kRunSingle:
        for (uint32_t k = 0; k < count; ++k) {
          const uint32_t reg = (tag & 0xFF) == kRun ? p[1] + k : p[1];
          if (!((reg >= 0x4800 && reg < 0x4900) || reg == 0x2318)) continue;
          if (in(p[2 + k])) {
            *what = fmt::format("register {:04X} = {:08X} (scratch {:08X}, physical {:08X})",
                                (tag & 0xFF) == kRun ? p[1] + k : p[1], p[2 + k], lo, plo);
            return true;
          }
        }
        p += 2 + count;
        break;
      case kType1:
        p += 4;
        break;
      case kType3: {
        const uint32_t opcode = (p[1] >> 8) & 0x7F;
        if (opcode == 0x2B || opcode == 0x2D || opcode == 0x10) {  // IM_LOAD_IMMEDIATE, SET_CONSTANT, NOP
          p += 3 + count;
          break;
        }
        for (uint32_t k = 0; k < count; ++k) {
          if (in(__builtin_bswap32(p[3 + k]))) {
            *what = fmt::format("packet {:08X} word {} = {:08X} (scratch {:08X}, physical {:08X})", p[1], k,
                                __builtin_bswap32(p[3 + k]), lo, plo);
            return true;
          }
        }
        p += 3 + count;
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

std::atomic<uint64_t> g_clears{0}, g_resolves{0};

bool RedirectedEntry(const Module& mod, PPCContext& ctx, uint8_t* base, GuestFn original, const char* what,
                     std::atomic<uint64_t>& count) {
  if (g_failed.load(std::memory_order_relaxed) || !me::native::NativeActive()) return false;
  ThreadData& t = Thread();
  if (t.in_original) return false;
  const uint32_t d = ctx.r3.u32;
  if (d < 0x40000000u || d >= 0x7F000000u || !Scratch()) return false;
  Mem m{base};
  if (m.L8(d + dev::kFlags) != 0) [[unlikely]] {  // tiling and the rest: never in the census
    Bump(g_fallbacks[kFbFlags]);
    return false;
  }
  static const bool verify = REXCVAR_GET(futari_cpu_verify);
  Shadow shadow;
  if (verify) [[unlikely]] shadow = RunShadow(t, mod, base, ctx, d, original);
  Ops ops = OpenOps(t);
  Ctx c{mod, m, ops, base, ctx, d, t};
  // The marker first, in the real command buffer (Direct3D's own space check): the inline-data allocator, which runs on
  // the real command buffer during the call (RedirectedInlineAlloc), then places its data past it. Its number is
  // filled in once the record is published (no submission can happen in between: SpaceDuringRedirect).
  const uint32_t w0 = MarkerSpace(c);
  const uint32_t marker_end = PutMarker(base, w0, 0);
  m.S32(d + dev::kWptr, marker_end);
  const uint32_t s = Scratch();
  const uint32_t w = m.L32(d + dev::kWptr), lim = m.L32(d + dev::kLimit), soft = m.L32(d + dev::kSoftLimit);
  g_real_w = w;
  g_real_lim = lim;
  g_real_soft = soft;
  m.S32(d + dev::kWptr, s);
  m.S32(d + dev::kLimit, s + kRedirectSize - 0x100);
  m.S32(d + dev::kSoftLimit, s + kRedirectSize - 0x1000);
  t.in_original = true;
  g_redirecting.store(true, std::memory_order_relaxed);
  PPCContext call = ctx;  // the caller's whole context: Clear passes its depth in f1
  original(call, base);
  g_redirecting.store(false, std::memory_order_relaxed);
  t.in_original = false;
  const uint32_t end = m.L32(d + dev::kWptr);
  const bool decoded = end <= s || DecodeInto(ops, base, s + 4, end);
  RestoreLimits(m, d, w, lim, soft, s);
  ctx.r3 = call.r3;
  std::string reference;
  if (!decoded || ScratchReference(ops, s, &reference)) [[unlikely]] {
    if (decoded) Bump(g_redirect_scratch_refs);
    REXLOG_ERROR("[api] {}: {}; the native layer is off from now on (this call's packets are lost)", what,
                 decoded ? "a packet points into the command buffer itself (data placed among the packets): " + reference
                         : std::string("its packets could not become ops"));
    g_failed.store(true, std::memory_order_relaxed);
    return true;
  }
  static int dumped = 0;
  if (dumped < 4) {  // the first records of each redirected entry point, as the ring will run them
    ++dumped;
    Canon canon;
    CanonOps(canon, ops.buf, ops.size());
    REXLOG_INFO("[api] {} record ({} op words; scratch {:08X}-{:08X}):{}", what, ops.size(), s, end, Describe(canon));
  }
  uint32_t seq = 0;
  if (!Publish(seq, ops)) return true;  // logged; the layer stops (the marker names record 0: not found, reported)
  rex::memory::store_and_swap<uint32_t>(HostOf(base, w0 + 12), seq);
  Bump(count);
  if (shadow.ok)
    CompareShadow(t, what, base, d, shadow, ops,
                  rex::memory::load_and_swap<uint32_t>(HostOf(base, shadow.copy + dev::kWptr)));
  return true;
}

}  // namespace

bool BeginVertices(const Module& module, PPCContext& ctx, uint8_t* base) {
  return Timed([&] { return BeginVerticesNative(module, ctx, base); });
}

bool WindowScissor(const Module& module, PPCContext& ctx, uint8_t* base) {
  return Timed([&] { return WindowScissorNative(module, ctx, base); });
}

bool Clear(const Module& module, PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(futari_api_clear);
  return on && Timed([&] { return RedirectedEntry(module, ctx, base, module.clear, "Clear", g_clears); });
}

bool Resolve(const Module& module, PPCContext& ctx, uint8_t* base) {
  static const bool on = REXCVAR_GET(futari_api_resolve);
  return on && Timed([&] { return RedirectedEntry(module, ctx, base, module.resolve, "Resolve", g_resolves); });
}

bool SpaceDuringRedirect(PPCContext& ctx, uint8_t* base) {
  if (!g_redirecting.load(std::memory_order_relaxed)) [[likely]] return false;
  // Keep writing in the scratch area; the submission the caller wanted is made on the real command buffer at its next
  // space check (RestoreLimits).
  Mem m{base};
  const uint32_t d = ctx.r3.u32, s = g_scratch;
  g_redirect_kick = true;
  m.S32(d + dev::kLimit, s + kRedirectSize - 0x100);
  m.S32(d + dev::kSoftLimit, s + kRedirectSize - 0x1000);
  ctx.r3.u64 = m.L32(d + dev::kWptr);
  return true;
}

bool RedirectedInlineAlloc(PPCContext& ctx, uint8_t* base, GuestFn original) {
  if (!g_redirecting.load(std::memory_order_relaxed)) [[likely]] return false;
  Mem m{base};
  const uint32_t d = ctx.r3.u32, words = ctx.r4.u32;
  const uint32_t sw = m.L32(d + dev::kWptr), slim = m.L32(d + dev::kLimit), ssoft = m.L32(d + dev::kSoftLimit);
  m.S32(d + dev::kWptr, g_real_w);
  m.S32(d + dev::kLimit, g_real_lim);
  m.S32(d + dev::kSoftLimit, g_real_soft);
  original(ctx, base);
  g_real_w = m.L32(d + dev::kWptr);
  g_real_lim = m.L32(d + dev::kLimit);
  g_real_soft = m.L32(d + dev::kSoftLimit);
  m.S32(d + dev::kWptr, sw);
  m.S32(d + dev::kLimit, slim);
  m.S32(d + dev::kSoftLimit, ssoft);
  Bump(g_redirect_allocs);
  NoteInlineAlloc(ctx.r3.u32, words);
  return true;
}

void NoteInlineAlloc(uint32_t address, uint32_t words) {
  static const bool verify = REXCVAR_GET(futari_cpu_verify);
  if (!verify || !address || !g_redirecting.load(std::memory_order_relaxed)) [[likely]] return;
  Thread().native_allocs.push_back({address, words});
}

bool ShadowInlineAlloc(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!g_shadow_running.load(std::memory_order_relaxed)) [[likely]] return false;
  ThreadData& t = Thread();
  if (!t.in_original) return false;
  // The shadow buffer's unused upper half, one block per allocation (aligned as asked), recorded in order.
  const uint32_t words = ctx.r4.u32, align = std::max<uint32_t>(4, ctx.r5.u32);
  uint32_t a = (t.shadow_bump + align - 1) & ~(align - 1);
  if (a + words * 4 > g_shadow + kShadowCopy + kShadowBuffer - 8) a = g_shadow + kShadowCopy + kShadowBuffer / 2;
  t.shadow_bump = a + words * 4;
  t.shadow_allocs.push_back({a, words});
  ctx.r3.u64 = a;
  return true;
}

void Report() {
  const uint64_t native = g_native.load(std::memory_order_relaxed);
  const uint64_t original = g_original.load(std::memory_order_relaxed);
  std::string fb;
  for (uint32_t i = 1; i < kFbCount; ++i)
    if (const uint64_t n = g_fallbacks[i].load(std::memory_order_relaxed)) fb += fmt::format(" {} {};", kFallbackNames[i], n);
  if (!native && !original && fb.empty()) return;
  static uint64_t last_checked = 0, last_differences = 0;
  const uint64_t checked = g_checked.load(std::memory_order_relaxed), differences = g_differences.load(std::memory_order_relaxed);
  REXLOG_INFO("[api] native records (futari_api_*, cumulative): {} ({:.1f} op words each; {} window scissor, {} without "
              "inline space; shader loads native {}; Direct3D's own helper redirected: shader load {}, render "
              "backend re-send {}; clears {}, resolves {}; submissions handed to the real command buffer {}), "
              "{} originals run in verify mode (on a device copy); fallbacks to the original:{}; verify: {} compared, {} "
              "differences{}",
              native, native ? double(g_ops_words.load(std::memory_order_relaxed)) / double(native) : 0.0,
              g_window.load(std::memory_order_relaxed), g_no_space.load(std::memory_order_relaxed),
              g_native_shader.load(std::memory_order_relaxed), g_redirect_shader.load(std::memory_order_relaxed),
              g_redirect_rb.load(std::memory_order_relaxed), g_clears.load(std::memory_order_relaxed),
              g_resolves.load(std::memory_order_relaxed), g_redirect_kicks.load(std::memory_order_relaxed), original, fb.empty() ? " none" : fb, checked, differences,
              g_failed.load(std::memory_order_relaxed) ? " (STOPPED)" : "");
  futari::native::ReportVerify("native Direct3D", checked - last_checked, differences - last_differences);
  last_checked = checked;
  last_differences = differences;
  static uint64_t last_calls = 0, last_ns = 0, last_timed = 0;
  const uint64_t calls = g_calls.load(std::memory_order_relaxed), ns = g_timed_ns.load(std::memory_order_relaxed),
                 timed = g_timed_calls.load(std::memory_order_relaxed);
  const uint64_t dc = calls - last_calls, dns = ns - last_ns, dt = timed - last_timed;
  last_calls = calls;
  last_ns = ns;
  last_timed = timed;
  if (dt) {
    const double per_ns = double(dns) / double(dt);
    const double total_ms = per_ns * double(dc) / 1e6;
    REXLOG_INFO("[api] drawing-thread cost (10 s): {:.2f} us per native call (every 16th timed: {} of {}), {:.1f} ms in "
                "all = {:.2f} % of one core",
                per_ns / 1000.0, dt, dc, total_ms, total_ms / 100.0);
  }
}

}  // namespace futari::api

// The game cores' hooks (src/native/futari_native_core.cpp) reach the native layer through these (GetProcAddress on the
// PC, the final link on the Switch).
#if defined(_WIN32)
#define FUTARI_API_EXPORT extern "C" __declspec(dllexport)
#else
#define FUTARI_API_EXPORT extern "C" __attribute__((visibility("default")))
#endif
FUTARI_API_EXPORT bool FutariApiBeginVertices(const futari::api::Module* module, PPCContext* ctx, uint8_t* base) {
  return futari::api::BeginVertices(*module, *ctx, base);
}
FUTARI_API_EXPORT bool FutariApiWindowScissor(const futari::api::Module* module, PPCContext* ctx, uint8_t* base) {
  return futari::api::WindowScissor(*module, *ctx, base);
}
FUTARI_API_EXPORT bool FutariApiClear(const futari::api::Module* module, PPCContext* ctx, uint8_t* base) {
  return futari::api::Clear(*module, *ctx, base);
}
FUTARI_API_EXPORT bool FutariApiResolve(const futari::api::Module* module, PPCContext* ctx, uint8_t* base) {
  return futari::api::Resolve(*module, *ctx, base);
}
FUTARI_API_EXPORT bool FutariApiSpaceDuringRedirect(PPCContext* ctx, uint8_t* base) {
  return futari::api::SpaceDuringRedirect(*ctx, base);
}
FUTARI_API_EXPORT bool FutariApiRedirectedInlineAlloc(PPCContext* ctx, uint8_t* base, futari::api::GuestFn original) {
  return futari::api::RedirectedInlineAlloc(*ctx, base, original);
}
FUTARI_API_EXPORT void FutariApiNoteInlineAlloc(uint32_t address, uint32_t words) {
  futari::api::NoteInlineAlloc(address, words);
}
FUTARI_API_EXPORT bool FutariApiShadowInlineAlloc(PPCContext* ctx, uint8_t* base) {
  return futari::api::ShadowInlineAlloc(*ctx, base);
}
