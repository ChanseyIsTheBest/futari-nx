/**
 * Switch: the console's shared "Standard" font (Latin + Japanese kana and kanji) for the ImGui overlay.
 *
 * The overlay's own font (ProggyTiny) covers Latin only, so the game's own strings in the achievement toasts and
 * menus (Mushihimesama Futari's achievements are Japanese) came out as "??????". The Windows build merges MS Gothic
 * for them (imgui_drawer.cpp); the Switch build merges this one.
 *
 * pl:u maps the shared fonts into shared memory. Depending on the firmware the payload is plain TrueType or BFTTF
 * (an sfnt XOR'd with a repeating 32-bit key behind an 8-byte header), so the key is derived from known plaintext
 * instead of a remembered constant: every sfnt starts with a version tag from a small set, and a candidate key is
 * accepted only when the decoded table directory is self-consistent (searchRange / entrySelector / rangeShift follow
 * from numTables). The method is hitmango_nx's (source/nx_fonts.c).
 *
 * The font is copied out, so pl:u is closed again at once.
 */

#include <switch.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rex::ui::nx {
namespace {

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Is `header` (12 bytes: version, numTables, searchRange, entrySelector, rangeShift) a plausible sfnt header?
bool SfntHeaderOk(const uint8_t* header, size_t available) {
  if (available < 12) return false;
  const uint32_t version = Be32(header);
  if (version != 0x00010000u && version != 0x4F54544Fu /* OTTO */ && version != 0x74727565u /* true */) return false;
  const uint32_t tables = (uint32_t(header[4]) << 8) | header[5];
  if (tables == 0 || tables > 512) return false;
  const uint32_t search_range = (uint32_t(header[6]) << 8) | header[7];
  const uint32_t entry_selector = (uint32_t(header[8]) << 8) | header[9];
  const uint32_t range_shift = (uint32_t(header[10]) << 8) | header[11];
  uint32_t power = 1, selector = 0;
  while (power * 2 <= tables) {
    power *= 2;
    ++selector;
  }
  return search_range == power * 16 && entry_selector == selector && range_shift == tables * 16 - search_range;
}

// The offset at which the TrueType data begins (decoded in place), or -1.
long DecodeFont(uint8_t* data, size_t size) {
  static constexpr size_t kOffsets[] = {8, 0};  // 8: the BFTTF header; 0: a raw font
  for (const size_t offset : kOffsets)          // already plain?
    if (size > offset + 12 && SfntHeaderOk(data + offset, size - offset)) return long(offset);
  static constexpr uint32_t kTags[] = {0x00010000u, 0x4F54544Fu, 0x74727565u};
  for (const size_t offset : kOffsets) {
    if (size < offset + 16) continue;
    for (const uint32_t tag : kTags) {
      const uint32_t key = Be32(data + offset) ^ tag;
      if (!key) continue;
      uint8_t probe[12];
      for (int i = 0; i < 12; ++i) probe[i] = uint8_t(data[offset + i] ^ uint8_t(key >> (24 - 8 * (i & 3))));
      if (!SfntHeaderOk(probe, sizeof(probe))) continue;
      for (size_t i = offset; i + 3 < size; i += 4) {
        data[i + 0] ^= uint8_t(key >> 24);
        data[i + 1] ^= uint8_t(key >> 16);
        data[i + 2] ^= uint8_t(key >> 8);
        data[i + 3] ^= uint8_t(key);
      }
      return long(offset);
    }
  }
  return -1;
}

}  // namespace

// The shared Standard font as a TrueType file in `ttf`. False (and `why` set) when it is unavailable.
bool LoadSharedFontStandard(std::vector<uint8_t>& ttf, const char** why) {
  if (R_FAILED(plInitialize(PlServiceType_User))) {
    *why = "pl:u unavailable";
    return false;
  }
  bool ok = false;
  PlFontData font{};
  if (R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) && font.address && font.size >= 16) {
    std::vector<uint8_t> data(static_cast<const uint8_t*>(font.address),
                              static_cast<const uint8_t*>(font.address) + font.size);
    const long offset = DecodeFont(data.data(), data.size());
    if (offset >= 0) {
      ttf.assign(data.begin() + offset, data.end());
      ok = true;
    } else {
      *why = "the shared font is not a TrueType font (BFTTF decode failed)";
    }
  } else {
    *why = "the shared Standard font is unavailable";
  }
  plExit();
  return ok;
}

}  // namespace rex::ui::nx
