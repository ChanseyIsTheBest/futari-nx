// The undo log of the localized copies' record mode (futari_native_hot_record.h fills it, the guard in
// futari_native_hot.cpp rolls it back).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace futari::hot {

struct Undo {
  uint8_t* at;
  uint32_t bytes;
  uint8_t old[16];
};

inline thread_local std::vector<Undo> g_undo;
inline thread_local bool g_undo_overflow = false;

inline uint8_t* Touch(uint8_t* at, uint32_t bytes) {
  if (g_undo.size() < (1u << 20)) {
    Undo u{at, bytes, {}};
    std::memcpy(u.old, at, bytes);
    g_undo.push_back(u);
  } else {
    g_undo_overflow = true;  // too much to roll back: this check is skipped
  }
  return at;
}

}  // namespace futari::hot
