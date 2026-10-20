// futari_cpu_direct_state (the direct renderer, step 1; me_native_system.cpp, namespace direct): default.xex's draw
// functions. Futari draws through Direct3D's BeginVertices / BeginIndexedVertices (DrawVerticesUP and
// DrawIndexedVerticesUP call them too): each flushes the dirty state as PM4 packets, writes the draw packet and returns
// the vertex (and index) memory the caller fills. The draw packet's last word is at device+13652; device+48 stays at the
// state packets' end until EndVertices (sub_8209D7F0 / sub_8209DC98) copies +13652 into it. A failed allocation returns
// 0 (BeginIndexedVertices: an error HRESULT) with no draw packet. The game cores have their own copy of BeginVertices
// (src/native/futari_native_core.cpp).
//
// Every hook calls the original; the only guest memory written is the marker NOP's three words over the state packets'
// first three, which no guest code reads back.

#include "me_native_system.h"
#include "futari_d3d_census.h"
#include "futari_api.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include "native/futari_native.h"
#include "renut_engine/futari_renut_yield.h"

namespace {

constexpr uint32_t kDeviceDrawEnd = 13652;  // device field: the last word of the draw packet BeginVertices wrote

// The code check of the framework (src/native/futari_native.h): only the bytes these hooks were written for.
bool SameCode(const uint8_t* base, const char* what, uint32_t address, uint32_t words, uint64_t expected) {
  const uint64_t found = futari::native::CodeHash(base, address, words);
  if (found != expected) {
    REXLOG_WARN("[native] {} at {:08X}: guest code differs (hash {:016X}, written for {:016X}): no direct state for "
                "its draws",
                what, address, found, expected);
    return false;
  }
  return true;
}

}  // namespace

// futari_api_draws (src/me/futari_api.cpp): default.xex's own Direct3D functions the native BeginVertices calls, and its
// globals (interpolator mask, its init flag, the declaration ids at 0x821ED334..0x821ED33F).
REX_EXTERN(__imp__sub_8209D800);
REX_EXTERN(__imp__sub_820A8138);
REX_EXTERN(__imp__sub_820A8FC0);
REX_EXTERN(__imp__sub_8209FC30);
REX_EXTERN(__imp__sub_8209E6D8);
REX_EXTERN(__imp__sub_82096B28);
REX_EXTERN(__imp__sub_8209E4E0);
REX_EXTERN(__imp__sub_8209FAF0);
REX_EXTERN(__imp__sub_8209F858);
REX_EXTERN(__imp__sub_820A19F0);
REX_EXTERN(__imp__sub_820A2280);
namespace {
const futari::api::Module kDefaultModule{"default",
                                         &__imp__sub_8209D800,
                                         &__imp__sub_820A8138,
                                         &__imp__sub_820A8FC0,
                                         &__imp__sub_8209FC30,
                                         &__imp__sub_8209E6D8,
                                         &__imp__sub_82096B28,
                                         &__imp__sub_8209E4E0,
                                         &__imp__sub_8209FAF0,
                                         &__imp__sub_8209F858,
                                         &__imp__sub_820A19F0,
                                         &__imp__sub_820A2280,
                                         0x821ED334u};
}  // namespace

REX_HOOK_RAW(sub_8209D800) {  // D3DDevice_BeginVertices (dev, primitive type, vertex count, stride) -> vertices
  FUTARI_RENUT_YIELD(D3DDevice_BeginVertices)  // Renut's renderer (src/renut_engine)
  static const bool same = SameCode(base, "BeginVertices", 0x8209D800u, 293, 0x702EF0A00981EF25ull);
  if (futari::d3d::CensusOn()) [[unlikely]] {  // futari_diag_d3d_census: counted, nothing converted
    futari::d3d::NoteDraw(base, ctx.r3.u32);
    futari::d3d::CensusEnter(0x8209D800, base, "default");
    __imp__sub_8209D800(ctx, base);
    futari::d3d::CensusLeave(base);
    return;
  }
  if (!same) return __imp__sub_8209D800(ctx, base);
  if (futari::api::BeginVertices(kDefaultModule, ctx, base)) return;  // futari_api_draws
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32, kDeviceDrawEnd);
  __imp__sub_8209D800(ctx, base);
  if (!ctx.r3.u32) direct.device = 0;  // nothing allocated: no draw packet (device+13652 is an old draw's)
  me::native::EndDrawDirect(base, direct);
}

REX_EXTERN(__imp__sub_8209DCE0);
REX_HOOK_RAW(sub_8209DCE0) {  // D3DDevice_BeginIndexedVertices -> HRESULT (indices, vertices through pointers)
  FUTARI_RENUT_ORIGINAL(sub_8209DCE0)  // Renut's renderer has no BeginIndexedVertices: Direct3D's own
  static const bool same = SameCode(base, "BeginIndexedVertices", 0x8209DCE0u, 326, 0xB459438DC3CBAF5Dull);
  if (futari::d3d::CensusOn()) [[unlikely]] {  // futari_diag_d3d_census: counted, nothing converted
    futari::d3d::NoteDraw(base, ctx.r3.u32);
    futari::d3d::CensusEnter(0x8209DCE0, base, "default");
    __imp__sub_8209DCE0(ctx, base);
    futari::d3d::CensusLeave(base);
    return;
  }
  if (!same) return __imp__sub_8209DCE0(ctx, base);
  auto direct = me::native::BeginDrawDirect(base, ctx.r3.u32, kDeviceDrawEnd);
  __imp__sub_8209DCE0(ctx, base);
  if (ctx.r3.u32 != 0) direct.device = 0;  // E_OUTOFMEMORY: no draw packet (device+13652 is an old draw's)
  me::native::EndDrawDirect(base, direct);
}

// Direct3D's inline-data allocator (device, words, alignment): verify mode's shadow runs of the native layer get their
// allocations from a scratch area (futari::api::ShadowInlineAlloc); every other call is the original.
REX_HOOK_RAW(sub_820A8138) {
  FUTARI_RENUT_YIELD(D3D_CDevice_AllocateInlineData)
  if (futari::api::ShadowInlineAlloc(ctx, base)) return;
  if (futari::api::RedirectedInlineAlloc(ctx, base, &__imp__sub_820A8138)) return;
  const uint32_t words = ctx.r4.u32;
  __imp__sub_820A8138(ctx, base);
  futari::api::NoteInlineAlloc(ctx.r3.u32, words);
}

// The window offset / scissor writer (device, x0, y0, x1, y1): futari_api_window_scissor.
REX_HOOK_RAW(sub_82096B28) {
  FUTARI_RENUT_YIELD(D3D_SetSurfaceClip)
  if (futari::api::WindowScissor(kDefaultModule, ctx, base)) return;
  __imp__sub_82096B28(ctx, base);
}

// D3DDevice_Clear and D3DDevice_Resolve through the records (futari_api_clear / futari_api_resolve).
REX_HOOK_RAW(sub_820A19F0) {
  FUTARI_RENUT_YIELD(D3DDevice_Clear)
  if (futari::api::Clear(kDefaultModule, ctx, base)) return;
  __imp__sub_820A19F0(ctx, base);
}
REX_HOOK_RAW(sub_820A2280) {
  FUTARI_RENUT_YIELD(D3DDevice_Resolve)
  if (futari::api::Resolve(kDefaultModule, ctx, base)) return;
  __imp__sub_820A2280(ctx, base);
}

// The command buffer's space function: no submission while Direct3D code runs on the native layer's scratch area.
REX_EXTERN(__imp__sub_820A8FC0);
REX_HOOK_RAW(sub_820A8FC0) {
  FUTARI_RENUT_YIELD(D3D_CDevice_KickOff)
  if (futari::api::SpaceDuringRedirect(ctx, base)) return;
  __imp__sub_820A8FC0(ctx, base);
}
