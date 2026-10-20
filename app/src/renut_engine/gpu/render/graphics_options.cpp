/**
 * @file    gpu/render/graphics_options.cpp
 *
 * futari-nx (2026-10-19): Futari's version. N&B's per-target choices (its
 * shadow maps, SSAO and post-processing buffers, recognised by their sizes and
 * EDRAM bases) are not Futari's: every target renders at the render
 * resolution in the host format of its own format, and the one option kept is
 * the anisotropy floor.
 */
#include "renut_engine/gpu/render/graphics_options.h"

#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/types.h>

#include "renut_engine/gpu/guest/widescreen.h"
#include "renut_engine/gpu/render/format.h"

REXCVAR_DEFINE_STRING(renut_anisotropic_filtering, "Game", "Futari/Renut",
                      "Anisotropic texture filtering: the game's own, or at least 2x, 4x, 8x or "
                      "16x.")
    .allowed({"Game", "2x", "4x", "8x", "16x"});

namespace renut::gpu {

namespace {
uint32_t g_min_anisotropy = 1;
}  // namespace

uint32_t GuestScreenWidth() {
  return 1280;
}

void LatchGraphicsOptionsLocked() {
  const int anisotropy = std::atoi(REXCVAR_GET(renut_anisotropic_filtering).c_str());
  g_min_anisotropy = anisotropy >= 2 ? uint32_t(anisotropy) : 1;
}

RenderScale SurfaceRenderScale(const d3d::SurfaceDesc & /*desc*/, RenderScale resolution) {
  return resolution;
}

plume::RenderFormat SurfaceColorFormat(const d3d::SurfaceDesc &desc) {
  return ColorTargetFormat(GpuColorFormat((u32(desc.ColorInfo) >> 16) & 0xF));
}

uint32_t MinAnisotropy() {
  return g_min_anisotropy;
}

}  // namespace renut::gpu
