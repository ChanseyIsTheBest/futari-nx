// Native replacements of the XDK C runtime functions every module carries (default.xex and each game core link their
// own copy): shared bodies, used by futari_native_crt.cpp (default.xex) and futari_native_core.cpp (cores).

#pragma once

#include "futari_native.h"

namespace futari::native::crt {

// memcpy(r3 = destination, r4 = source, r5 = size) -> r3 = destination. The XDK's copy (290 instructions, the same
// bytes in all nine modules) is a pure forward copy (byte/word/doubleword/128-byte loops with dcbt/dcbtst hints, no
// dcbz) that saves r3 in the red zone and reloads it: for ranges that do not overlap its result is exactly a host
// memcpy. Overlapping ranges (undefined for memcpy, but the forward copy gives a defined pattern) and ranges crossing
// into the physical views run the recompiled copy.
inline constexpr uint32_t kMemcpyWords = 290;
inline constexpr uint64_t kMemcpyHash = 0xEE0EAEB1E0584297ull;

template <void (*Original)(PPCContext&, uint8_t*)>
inline void Memcpy(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
  const bool apart = uint64_t(dst) + size <= src || uint64_t(src) + size <= dst;
  if (!apart || !Contiguous(dst, size) || !Contiguous(src, size)) {
    Original(ctx, base);
    return;
  }
  std::memcpy(Host(base, dst), Host(base, src), size);
}

template <void (*Original)(PPCContext&, uint8_t*)>
inline void MemcpySite(Site& site, PPCContext& __restrict ctx, uint8_t* base) {
  switch (ModeOf(site, base)) {
    case kOriginal:
      return Original(ctx, base);
    case kNative:
      return Memcpy<Original>(ctx, base);
    case kVerify:
      if (!Contiguous(ctx.r3.u32, ctx.r5.u32)) {
        return Original(ctx, base);
      }
      return Verify(site, ctx, base, {{ctx.r3.u32, ctx.r5.u32}}, Original, Memcpy<Original>);
  }
}

// memset(r3 = destination, r4 = value (its low byte), r5 = size) -> r3 = destination unchanged. The XDK's fill (40
// instructions, the same bytes in all nine modules; sa2-nx's n_827EFEA0): single bytes up to 4-byte alignment, the low
// byte of r4 replicated into a word, 16-byte blocks of four stw, then words and bytes. Every byte of [dst, dst + n) is
// written once with the low byte of r4, nothing outside it, nothing read: memset(dst, r4 & 0xFF, n). ctr = n + 1 is
// 64-bit, so a size with its upper half set (the head loop could run past n) runs the original, as do ranges crossing
// into the physical views. r0 / r4-r6 / ctr are the routine's scratch (no caller reads them after the call).
inline constexpr uint32_t kMemsetWords = 40;
inline constexpr uint64_t kMemsetHash = 0x54D201DC8A21DABAull;

template <void (*Original)(PPCContext&, uint8_t*)>
inline void Memset(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, size = ctx.r5.u32;
  if ((ctx.r5.u64 >> 32) || !Contiguous(dst, size)) {
    Original(ctx, base);
    return;
  }
  if (size) std::memset(Host(base, dst), int(ctx.r4.u32 & 0xFF), size);
}

template <void (*Original)(PPCContext&, uint8_t*)>
inline void MemsetSite(Site& site, PPCContext& __restrict ctx, uint8_t* base) {
  switch (ModeOf(site, base)) {
    case kOriginal:
      return Original(ctx, base);
    case kNative:
      return Memset<Original>(ctx, base);
    case kVerify:
      if ((ctx.r5.u64 >> 32) || !Contiguous(ctx.r3.u32, ctx.r5.u32)) {
        return Original(ctx, base);
      }
      return Verify(site, ctx, base, {{ctx.r3.u32, ctx.r5.u32}}, Original, Memset<Original>);
  }
}

}  // namespace futari::native::crt
