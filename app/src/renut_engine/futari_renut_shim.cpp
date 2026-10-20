// futari-nx (2026-10-19): what Renut's renderer (banjo-nx's copy, app/src/renut_engine/gpu) takes from the rest of
// banjo-nx: its enhancement switches (on Futari's, src/cpu/banjo_enhancements.h) and the keys banjo-nx defines outside
// the renderer, here with banjo-nx's names and defaults (Futari has no other renderer reading them).
#include "cpu/banjo_enhancements.h"

#include <cstdint>

#include <rex/cvar.h>

#include "futari_enhancements.h"
#include "native/futari_native.h"

// futari-nx (2026-10-20, input latency, the user's request through banjo-nx): a 2-image swapchain (backend/video.cpp).
// With 3 (the frames in flight + 1) a finished image can wait behind another for the display; with 2 the GPU's next
// frame waits for the display to give back the one before (dkQueueAcquireImage is a queue wait, not the render
// thread's), one image less queued before it is shown. A frame that misses a refresh costs a whole refresh there.
REXCVAR_DEFINE_BOOL(futari_renut_double_buffer, true, "Futari/Renut",
                    "Renut's renderer: the swapchain with 2 images (double buffering: one image less queued before "
                    "the display) instead of 3")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Renderer threads (banjo_renut_tuning.cpp): the cores they are pinned to.
REXCVAR_DEFINE_INT32(banjo_ring_core, 1, "Futari/Renut",
                     "Renut's renderer: its render thread is pinned to this core (-1 = the process's whole mask)")
    .range(-1, 3);
REXCVAR_DEFINE_INT32(banjo_background_compile_core, 2, "Futari/Renut",
                     "Renut's renderer: the background compile threads (pipeline prewarm, bool specialisations) are "
                     "pinned to this core (-1 = the process's whole mask)")
    .range(-1, 3);
REXCVAR_DEFINE_INT32(banjo_prewarm_threads, 1, "Futari/Renut",
                     "Renut's renderer: threads that prewarm the pipeline list, all at the lowest priority")
    .range(1, 3);
REXCVAR_DEFINE_BOOL(banjo_cpu_verify, false, "Futari/Renut",
                    "Verify configuration of Renut's renderer: its CPU enhancements check themselves against the "
                    "original path and the log reports '[verify] ... differences: N' (must be 0). Slower; for testing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// GPU (non-CPU enhancements) of Renut's pipelines (banjo-nx's keys, shared there with its native renderer).
REXCVAR_DEFINE_BOOL(banjo_gpu_blend_skip, true, "Futari/Renut",
                    "GPU (non-CPU enhancement): transparent pixels whose blend leaves the RGBA8 target unchanged are "
                    "discarded before the blend (with renut's own banjo_renut_blend_skip)");
REXCVAR_DEFINE_BOOL(banjo_gpu_bool_spec, true, "Futari/Renut",
                    "GPU (non-CPU enhancement): a pipeline is created with the values of the bool constants its shaders "
                    "test (specialisation constants), so the code behind a false bool is not compiled");
REXCVAR_DEFINE_BOOL(banjo_gpu_tess_grid, true, "Futari/Renut",
                    "GPU (non-CPU enhancement): tessellated triangle patches drawn as a precomputed grid");
REXCVAR_DEFINE_BOOL(banjo_gpu_robustness_off, true, "Futari/Renut",
                    "GPU (non-CPU enhancement): the game's pipelines are created with buffer and image robustness "
                    "disabled (VK_EXT_pipeline_robustness)");
REXCVAR_DEFINE_BOOL(banjo_gpu_nvk_draw_contract, true, "Futari/Renut",
                    "GPU / driver (non-CPU enhancement): NVK's per-draw contract (Vulkan back end on the Switch)");

// NVK (the Vulkan back end on the Switch; masseffect-nx's keys, read by Renut's tuning).
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_emission, true, "Futari/Renut",
                    "NVK writes each draw command in one go, with the same bytes (self-checked in NVK)");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_cbufs, true, "Futari/Renut",
                    "NVK only looks at the cbuf slots that can get dirty and skips the cbuf flush when nothing is dirty");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_dynamic, true, "Futari/Renut",
                    "NVK only emits the dynamic state groups with dirty bits");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_preload, true, "Futari/Renut",
                    "Cache hints (PRFM) for the pipeline and shaders before they are used");
REXCVAR_DEFINE_INT32(masseffect_native_nvk_measure, 0, "Futari/Renut",
                     "Measures by parts inside NVK on 1 in N calls (power of 2). 0 = no measuring")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(masseffect_native_set4_differences, true, "Futari/Renut",
                    "Descriptor set 4 (UBO constants) by differences in NVK");

// banjo-nx's store cards (banjo_renut_store_cards) need its codegen pass, every guest store marking its 4 KB page
// (tools/store_cards.py): Futari's generated code marks none, so the renderer never trusts them.
extern "C" const bool g_banjo_store_cards = false;
// banjo-nx's SDK counts the host writers' marks for banjo_renut_store_cards_verify; Futari's marks none.
extern "C" uint64_t RexStoreCardsHostBytes(int /*which*/) { return 0; }

namespace banjo {

bool EnhancementActive(EnhancementGroup group, bool own_key) {
  return own_key && futari::enhancements::GroupOn(group == EnhancementGroup::kCpu
                                                      ? futari::enhancements::Group::kCpu
                                                      : futari::enhancements::Group::kNonCpu);
}

void CodegenChecksReport(int seconds) {
  futari::native::CodegenChecksReport(seconds);
}

}  // namespace banjo
