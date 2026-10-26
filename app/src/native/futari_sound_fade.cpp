// A sound the game cuts is faded out instead of stopping dead. Always on, no key (the user, 1.0.3): the Switch
// driver's output declick it replaced is off (futari_enhancements.cpp Apply).
//
// The crackle of Xbox 360 mode (docs/futari-notes.md "The crackle's cause"): the game plays each effect on one of its
// sound channels, and playing on a channel first destroys the cue still playing there (game core, reco_abnormal
// sub_88090588 -> sub_880901B8: UnRegisterNotification, IXACT3Cue::Destroy). XACT deletes the cue at once and XAudio2
// drops its voice from the next pass, so an effect cut 25-130 ms after its start (the xWMA effects are near full
// scale there) leaves a step on a 256-sample frame edge, up to 15 a second in busy play.
//
// Here IXACT3Cue::Destroy (default.xex sub_820EC8F8, every core calls it through the cue's vtable) gives the cue a
// zero output matrix first (IXACT3Cue::SetMatrixCoefficients, sub_820ECE08, the call the game sets its channel
// volumes with, so the counts are the game's own: recorded from its calls), returns S_OK, and destroys the cue
// kFadePasses XAudio2 passes later (the XAudio2 thread applies the new matrix across one pass). The game has already
// unregistered the cue's notification and forgotten its handle, so nothing else sees the difference. The deferred
// destroys run on the thread of the game's next Play or Destroy (the game's sound thread); every pending cue is
// destroyed at once before a sound bank is destroyed or the engine shuts down.

#include "../../generated/default/futari_pch.h"

#include <atomic>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "futari_native.h"

uint64_t FutariXAudio2Passes();  // futari_audio_diag.cpp

REX_EXTERN(__imp__sub_820EC8F8);  // IXACT3Cue::Destroy
REX_EXTERN(__imp__sub_820ECE08);  // IXACT3Cue::SetMatrixCoefficients(cue, source channels, destination channels, matrix)
REX_EXTERN(__imp__sub_820EEE00);  // IXACT3SoundBank::Play
REX_EXTERN(__imp__sub_820EEB60);  // IXACT3SoundBank::Destroy
REX_EXTERN(__imp__sub_820E6A98);  // IXACT3Engine::ShutDown (vtable 0x82013F48 slot 5)

namespace {

// The new matrix is committed at XACT's next update and rendered across the pass after it: 4 passes (21 ms) leave a
// pass of margin on each side.
constexpr uint64_t kFadePasses = 4;
// Matrix coefficients: XACT takes at most 8 source and 8 destination channels.
constexpr uint32_t kMaxCoefficients = 64;

struct Pending {
  uint32_t cue;
  uint64_t due;
};

std::mutex g_mutex;
std::vector<Pending> g_pending;
std::atomic<bool> g_any_pending{false};
std::atomic<uint32_t> g_source_channels{0};
std::atomic<uint32_t> g_destination_channels{0};
std::atomic<uint32_t> g_zeros{0};  // guest address of kMaxCoefficients zero floats
std::atomic<uint64_t> g_faded{0};

uint32_t Zeros() {
  uint32_t zeros = g_zeros.load(std::memory_order_acquire);
  if (!zeros) {
    auto* memory = rex::system::kernel_state()->memory();
    const uint32_t block = memory->SystemHeapAlloc(4 * kMaxCoefficients, 16);
    if (!block) return 0;
    std::memset(memory->TranslateVirtual(block), 0, 4 * kMaxCoefficients);  // 0.0f is zero in either byte order
    uint32_t expected = 0;
    if (!g_zeros.compare_exchange_strong(expected, block, std::memory_order_acq_rel)) {
      memory->SystemHeapFree(block);
      return expected;
    }
    zeros = block;
  }
  return zeros;
}

// Destroys the pending cues that are due (all of them with `all`). Clobbers the volatile registers.
void DestroyPending(PPCContext& ctx, uint8_t* base, bool all) {
  if (!g_any_pending.load(std::memory_order_acquire)) return;
  const uint64_t now = FutariXAudio2Passes();
  std::vector<uint32_t> due;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto keep = g_pending.begin();
    for (const Pending& p : g_pending) {
      if (all || p.due <= now) {
        due.push_back(p.cue);
      } else {
        *keep++ = p;
      }
    }
    g_pending.erase(keep, g_pending.end());
    g_any_pending.store(!g_pending.empty(), std::memory_order_release);
  }
  for (const uint32_t cue : due) {
    ctx.r3.u64 = cue;
    __imp__sub_820EC8F8(ctx, base);
  }
}

// Runs `original` with the registers it was called with, after the due destroys.
void AfterPending(PPCContext& ctx, uint8_t* base, bool all, void (*original)(PPCContext&, uint8_t*)) {
  if (g_any_pending.load(std::memory_order_acquire)) {
    const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64,
                   r8 = ctx.r8.u64, r9 = ctx.r9.u64, r10 = ctx.r10.u64;
    DestroyPending(ctx, base, all);
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    ctx.r8.u64 = r8;
    ctx.r9.u64 = r9;
    ctx.r10.u64 = r10;
  }
  original(ctx, base);
}

}  // namespace

// The game's own matrix calls give the channel counts the zero matrix needs.
REX_HOOK_RAW(sub_820ECE08) {
  const uint32_t sources = ctx.r4.u32, destinations = ctx.r5.u32, matrix = ctx.r6.u32;
  __imp__sub_820ECE08(ctx, base);
  if (ctx.r3.s32 >= 0 && matrix && sources && destinations && sources * destinations <= kMaxCoefficients) {
    g_source_channels.store(sources, std::memory_order_relaxed);
    g_destination_channels.store(destinations, std::memory_order_relaxed);
  }
}

REX_HOOK_RAW(sub_820EC8F8) {
  const uint32_t cue = ctx.r3.u32;
  DestroyPending(ctx, base, false);
  const uint32_t sources = g_source_channels.load(std::memory_order_relaxed);
  const uint32_t destinations = g_destination_channels.load(std::memory_order_relaxed);
  const uint32_t zeros = sources ? Zeros() : 0;
  if (zeros) {
    ctx.r3.u64 = cue;
    ctx.r4.u64 = sources;
    ctx.r5.u64 = destinations;
    ctx.r6.u64 = zeros;
    __imp__sub_820ECE08(ctx, base);
    if (ctx.r3.s32 >= 0) {
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending.push_back({cue, FutariXAudio2Passes() + kFadePasses});
        g_any_pending.store(true, std::memory_order_release);
      }
      if (g_faded.fetch_add(1, std::memory_order_relaxed) == 0) {
        REXLOG_INFO("[sound] cut sounds fade out over {} XAudio2 passes ({}x{} matrix)",
                    kFadePasses, sources, destinations);
      }
      ctx.r3.u64 = 0;  // S_OK
      return;
    }
  }
  ctx.r3.u64 = cue;
  __imp__sub_820EC8F8(ctx, base);
}

REX_HOOK_RAW(sub_820EEE00) { AfterPending(ctx, base, false, __imp__sub_820EEE00); }
REX_HOOK_RAW(sub_820EEB60) { AfterPending(ctx, base, true, __imp__sub_820EEB60); }
REX_HOOK_RAW(sub_820E6A98) { AfterPending(ctx, base, true, __imp__sub_820E6A98); }
