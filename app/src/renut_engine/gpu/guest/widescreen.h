/**
 * @file    gpu/guest/widescreen.h
 * @brief   The game's screen width.
 *
 * futari-nx: Futari's screen is 1280x720, always (N&B's widescreen hooks,
 * guest/widescreen.cpp of banjo-nx's copy, are not Futari's: render/
 * graphics_options.cpp defines this).
 */
#pragma once

#include <cstdint>

namespace renut::gpu {

// The game's screen width (1280; the height is 720).
uint32_t GuestScreenWidth();

} // namespace renut::gpu
