// banjo-nx: the boot-time choice between banjo-nx's native renderer and Renut's (banjo_renut_select.h); futari-nx
// (2026-10-19): Futari's key, futari_renderer.
#include "renut_engine/gpu/banjo_renut_select.h"

#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/backend/backend.h"     // futari-nx (peggle2-nx): IsDeko3D
#include "renut_engine/gpu/banjo_renut_deko3d.h"  // futari-nx (peggle2-nx): MissingGameShaders
#endif

REXCVAR_DEFINE_STRING(futari_renderer, "default", "Futari",
                      "Renderer, chosen at boot. default = Futari's own (futari_renderer_native chooses the native "
                      "PM4-ring renderer or the emulated Xenos GPU). renut = Renut's Direct3D-level renderer "
                      "(github.com/masterspike52/reNut, by reNut and masterspike52, through banjo-nx): Futari's "
                      "Direct3D runs natively through plume (Vulkan on the PC, deko3d on the Switch), with no PM4 ring "
                      "and no EDRAM model; its own renut_* keys apply")
    .allowed({"default", "renut"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace banjo::renut_gpu {

namespace detail {
int8_t g_active = 0;
bool LatchActive() {
  static const bool active = [] {
    const bool renut = REXCVAR_GET(futari_renderer) == "renut";
#if defined(PLUME_DEKO3D)
    // futari-nx (2026-10-20, peggle2-nx's): a build without translated game shaders (FUTARI_RENUT_RELEASE_SHADERS)
    // takes them from the web installer's deko3d_glsl.bin beside the NRO; without it nothing of the game is drawn.
    if (renut && ::renut::gpu::IsDeko3D()) {
      if (const size_t missing = deko3d::MissingGameShaders())
        REXLOG_ERROR("[deko3d] {} of the game's shaders are in no GLSL package: deko3d_glsl.bin is missing beside the "
                     "NRO (the web installer makes it from your game files); the game's pictures will not be drawn",
                     missing);
    }
#endif
    return renut;
  }();
  g_active = active ? 1 : -1;
  return active;
}
}  // namespace detail

}  // namespace banjo::renut_gpu

// The game cores' forwarding (futari_renut_core_hooks.cpp): the entries in FUTARI_RENUT_CORE_ENTRIES order while
// Renut's renderer is chosen, null otherwise (GetProcAddress on the PC, the final link on the Switch).
#if defined(_WIN32)
#define FUTARI_RENUT_EXPORT extern "C" __declspec(dllexport)
#else
#define FUTARI_RENUT_EXPORT extern "C" __attribute__((visibility("default")))
#endif
namespace {
using Entry = void (*)(PPCContext&, uint8_t*);
#define FUTARI_RENUT_ENTRY_POINTER(name) &futari_renut_entry_##name,
const Entry kCoreEntries[FUTARI_RENUT_CORE_COUNT + 1] = {FUTARI_RENUT_CORE_ENTRIES(FUTARI_RENUT_ENTRY_POINTER) nullptr};
#undef FUTARI_RENUT_ENTRY_POINTER
}  // namespace
FUTARI_RENUT_EXPORT const Entry* FutariRenutEntries(uint32_t* count) {
  if (count) *count = FUTARI_RENUT_CORE_COUNT;
  return banjo::renut_gpu::Active() ? kCoreEntries : nullptr;
}

// A core's entry that may free the game's own memory (D3DResource_Destroy / _Release): through the core's XMemFree,
// whose heap is the core's (d3d/hooks/resource.cpp GameXMemFree).
namespace renut::gpu {
extern thread_local PPCFunc* g_core_xmemfree;
}  // namespace renut::gpu
FUTARI_RENUT_EXPORT void FutariRenutCallWithFree(uint32_t index, PPCContext* ctx, uint8_t* base, PPCFunc* xmemfree) {
  PPCFunc* const was = renut::gpu::g_core_xmemfree;
  renut::gpu::g_core_xmemfree = xmemfree;
  kCoreEntries[index](*ctx, base);
  renut::gpu::g_core_xmemfree = was;
}
