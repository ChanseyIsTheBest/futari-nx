// Native replacement of Mushihimesama Futari's LZSS decompressor (every game core has the same 76 instructions:
// reco_abnormal sub_8813A120, the other cores in futari_native_core.cpp). Console profile 2026-10-06: 27 % of the busy
// gameMainThr in Xbox 360 mode (streamed data decompressed during play), one guest byte at a time.
//
// r3 = destination, r4 = packed block; returns r3 = the block's first word. Block, big-endian: [0] the returned word,
// [4] token count (signed; <= 0 = nothing), [8] offset of the data stream from the block. Flag bytes start at block +
// 12, most significant bit first, one bit per token: 1 = a 16-bit big-endian pair from the data stream, length
// (v & 31) + 3, distance v >> 5, copied byte by byte forwards from (output - distance), so an overlapping copy repeats
// the pattern; 0 = one literal byte from the data stream.

#pragma once

#include "futari_native.h"

namespace futari::native::lzss {

inline constexpr uint32_t kWords = 76;
inline constexpr uint64_t kHash = 0x79CB675FFA849635ull;

inline uint32_t Load32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Bytes the block writes (a dry run over the tokens), for the verify ranges.
inline uint32_t OutputBytes(uint8_t* base, uint32_t src) {
  const uint8_t* block = Host(base, src);
  int32_t tokens = int32_t(Load32(block + 4));
  const uint8_t* flags = block + 12;
  const uint8_t* data = Host(base, src + Load32(block + 8));
  uint32_t flag = 0, bits = 0, total = 0;
  for (; tokens > 0; --tokens) {
    if (bits == 0) {
      flag = *flags++;
      bits = 8;
    }
    if (flag & 0x80) {
      total += ((uint32_t(data[0]) << 8 | data[1]) & 31) + 3;
      data += 2;
    } else {
      total += 1;
      data += 1;
    }
    --bits;
    flag = (flag << 1) & 0xFF;
  }
  return total;
}

// One host pointer covers a buffer unless it could reach across 0xE0000000, where the PC keeps the physical views
// 0x1000 higher (Host()); the game decompresses into physical allocations (0xA0000000 and up).
inline bool AwayFromViewSeam(uint32_t address) {
  return address >= 0xE0000000u || address + (16u << 20) <= 0xE0000000u;
}

template <void (*Original)(PPCContext&, uint8_t*)>
inline void Decompress(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
  if (!AwayFromViewSeam(dst) || !AwayFromViewSeam(src)) {
    Original(ctx, base);
    return;
  }
  const uint8_t* block = Host(base, src);
  const uint32_t first = Load32(block);
  int32_t tokens = int32_t(Load32(block + 4));
  const uint8_t* flags = block + 12;
  const uint8_t* data = Host(base, uint32_t(src + Load32(block + 8)));
  uint8_t* out = Host(base, dst);
  uint32_t flag = 0, bits = 0;
  for (; tokens > 0; --tokens) {
    if (bits == 0) {
      flag = *flags++;
      bits = 8;
    }
    if (flag & 0x80) {
      const uint32_t v = uint32_t(data[0]) << 8 | data[1];
      data += 2;
      const uint32_t length = (v & 31) + 3, distance = v >> 5;
      const uint8_t* from = out - distance;
      if (distance >= length) {
        std::memcpy(out, from, length);
      } else {
        for (uint32_t i = 0; i < length; ++i) {
          out[i] = from[i];  // overlapping: each byte may be one just written
        }
      }
      out += length;
    } else {
      *out++ = *data++;
    }
    --bits;
    flag = (flag << 1) & 0xFF;
  }
  ctx.r3.u64 = first;
}

template <void (*Original)(PPCContext&, uint8_t*)>
inline void Site(futari::native::Site& site, PPCContext& __restrict ctx, uint8_t* base) {
  switch (ModeOf(site, base)) {
    case kOriginal:
      return Original(ctx, base);
    case kNative:
      return Decompress<Original>(ctx, base);
    case kVerify: {
      if (!AwayFromViewSeam(ctx.r3.u32) || !AwayFromViewSeam(ctx.r4.u32)) {
        return Original(ctx, base);
      }
      const uint32_t bytes = OutputBytes(base, ctx.r4.u32);
      return Verify(site, ctx, base, {{ctx.r3.u32, bytes}}, Original, Decompress<Original>);
    }
  }
}

}  // namespace futari::native::lzss
