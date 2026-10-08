// futari-nx: the native Direct3D layer (sa2-nx's API-level renderer, its docs/api-renderer.md; the user's request of
// 2026-10-07: remove the guest Direct3D, PM4 and EDRAM). Entry points of the game's Direct3D become native code that
// keeps the Direct3D device exactly as the game sees it and writes host records of operations (the direct-state ring of
// me_native_system.cpp, 'FTRA' markers) instead of PM4.
//
// Futari links Direct3D (XDK 2.0.9328) nine times: default.xex and each game core have their own copy, at their own
// addresses, with the same device layout. The native code is the executable's; a module describes itself with a Module
// (its own recompiled functions the native code calls, its Direct3D globals), and a core's hooks reach the native code
// through the executable's exports (src/native/futari_native_core.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace futari::api {

using GuestFn = void (*)(PPCContext&, uint8_t*);

struct Module {
  const char* name;
  GuestFn begin_vertices;  // the original D3DDevice_BeginVertices (verify mode's shadow runs)
  GuestFn alloc_inline;    // Direct3D's inline-data allocator (device, words, alignment) -> address or 0
  GuestFn space;           // the command buffer's space function (device) -> the write pointer after it
  GuestFn shader_load;     // the draw-time shader load (device, group mask) -> group mask
  GuestFn rb_resend;       // the draw-time re-send of 0x2203 / 0x2000 / 0x2104 (device, mask, reg 0x2203) -> mask
  GuestFn window_scissor;  // the original window offset / scissor writer (device, x0, y0, x1, y1)
  GuestFn depth_2203;      // the shader load's 0x2203 depth helper (device, mask) -> mask (writes packets)
  GuestFn binding_match;   // is a vertex shader binding patched for this declaration (device, vs, decl, idx) -> bool
  GuestFn bind_inline;     // a binding as an inline patched copy (device, relink, vs, &cntl, decl, vprog, pprog, idx)
  GuestFn clear;           // D3DDevice_Clear (device, count, rects, flags, color, z in f1, stencil)
  GuestFn resolve;         // D3DDevice_Resolve (device, flags, rect, texture, ...)
  uint32_t globals;        // Direct3D's interpolator mask, its init flag and the declaration ids (12 bytes)
};

// D3DDevice_BeginVertices(device, primitive type, vertex count, stride) natively: true when done (r3 = the vertex
// memory, or 0), false when the caller runs the original (key off, a state the native code does not cover).
bool BeginVertices(const Module& module, PPCContext& ctx, uint8_t* base);
// Direct3D's window offset / scissor writer (registers 0x2080-0x2082; SetViewport, SetScissorRect and the render target
// setters call it): natively, the same contract as BeginVertices.
bool WindowScissor(const Module& module, PPCContext& ctx, uint8_t* base);
// D3DDevice_Clear / D3DDevice_Resolve through the records (futari_api_clear / futari_api_resolve): Direct3D's own code
// runs with its command buffer pointed at a scratch area and what it wrote becomes a record (RedirectedEntry), so no PM4
// of theirs reaches the real command buffer (the transport switch's prerequisite). The same contract as BeginVertices.
bool Clear(const Module& module, PPCContext& ctx, uint8_t* base);
bool Resolve(const Module& module, PPCContext& ctx, uint8_t* base);
// The command buffer's space function, while Direct3D code runs on the scratch area (RedirectedCall / RedirectedEntry):
// no submission (it would publish the scratch area). True when answered: r3 = the write pointer, the scratch limits
// restored, and the "submit at the next space check" the inline allocator asked for kept for the real command buffer.
bool SpaceDuringRedirect(PPCContext& ctx, uint8_t* base);
// Direct3D's inline-data allocator while Direct3D code runs on the scratch area: it places its data in the command
// buffer's own memory (after the write pointer or the segment end, released as the GPU's read pointer passes), so it
// must see the real command buffer: its +48 / +52 / +56 are swapped in around the original. True when answered.
bool RedirectedInlineAlloc(PPCContext& ctx, uint8_t* base, GuestFn original);
// The inline-data allocator's result, after the original ran (verify mode notes the allocations a redirected call
// made, for CompareShadow's mapping onto the shadow run's).
void NoteInlineAlloc(uint32_t address, uint32_t words);
// Verify mode, inside an original's shadow run: the inline-data allocator answers from a scratch area (its copied
// bookkeeping could wait for GPU progress the copy never sees). True when answered.
bool ShadowInlineAlloc(PPCContext& ctx, uint8_t* base);
// The "[api]" report lines (the ring thread's 10 s report).
void Report();

}  // namespace futari::api
