// Native graphics system (see me_native_system.cpp).
#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace me::native {

// true when FUTARI_NATIVE=1: the app installs the native system instead of Xenos emulation.
bool Enabled();

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

// Called on the game thread when Direct3D binds a shader object (sub_8222F850), before it patches the
// vertex shader: records which library entry the microcode at that physical address belongs to, so the
// ring can identify IM_LOADs by address No-op without the native system.
void NoteShaderObject(const uint8_t* base, uint32_t object, bool vertex);

// True when the shader object was matched to a library entry (diagnostics).
bool HasEntry(uint32_t object);

// Called on the game thread by the Direct3D draw functions (the game's hooks file): the device they were called on
// (r3), the call's primitive type and counts. The record also takes the bound VS and PS objects from the device and
// its command-buffer position (device+48). The ring pairs each DRAW_INDX with these records in order, because the
// ring's own shader loads can be immediate copies Direct3D patched. No-op without the native system.
void NoteDrawCall(const uint8_t* base, uint32_t device, uint32_t type, uint32_t r5, uint32_t r6, uint32_t r7);

// Where the game's Direct3D device keeps the bound vertex and pixel shader objects; it moves between XDK versions
// (Mass Effect, the default: +12416 / +12412; Banjo-Kazooie N&B, 2008: +12696 / +12692; SA2, 2012: +13072 / +13068).
// Set once by the game's hooks file, before the first draw.
void SetDeviceShaderOffsets(uint32_t vertex_shader_offset, uint32_t pixel_shader_offset);

// Called by the ring thread after it writes the read pointer back: wakes the game's D3D GPU wait
// (src/native/me_ring_wait.cpp, sub_8222FA98).
void NotifyRingProgress();

// futari_cpu_direct_state (the direct renderer, step 1, ported from sa2-nx): the draw hooks call BeginDrawDirect before
// Direct3D's draw function and EndDrawDirect right after it. The state packets the draw function wrote before its draw
// packet become a host record the ring applies directly (no PM4 parsing for them), covered in the command buffer by one
// NOP packet. See namespace direct in me_native_system.cpp. No-op without the native system or with the key off.
// Futari's draws are Direct3D's BeginVertices / BeginIndexedVertices (src/native/futari_native_core.cpp,
// src/me/futari_direct_hooks.cpp): they write the draw packet but leave device+48 at the state packets' end; the draw
// packet's last word is at device+13652 (EndVertices copies it to +48), so end_offset says where the end is.
struct DrawDirect {
  uint32_t device = 0;       // 0 = nothing to do after the call
  uint32_t start = 0;        // Direct3D's command buffer write pointer before the call (the last word written)
  uint32_t limit = 0;        // the command buffer segment's limits before the call (device+52, +56): a segment
  uint32_t soft_limit = 0;   //   change during the call leaves the packets as they are
  uint32_t end_offset = 48;  // the device field that holds the last word written after the call
  uint32_t pad = 0;
  uint64_t submissions = 0;  // the ring write pointer's guest writes before the call
};
DrawDirect BeginDrawDirect(const uint8_t* base, uint32_t device, uint32_t end_offset = 48);

// The native renderer is up (its ring thread runs): the native Direct3D layer writes records only then.
bool NativeActive();

// In-run A/B measurement (futari_ab_s, futari_ab_key): true while the enhancement named `key` is in its "off" half.
bool AbOff(const char* key);
void EndDrawDirect(uint8_t* base, const DrawDirect& d);

// The native Direct3D layer (futari_api_*, src/me/futari_api_internal.h): a native call's ops become a record of the
// direct-state ring, named by a marker NOP [0xC0011000]['FTRA'][seq] the caller writes in fresh command buffer space.
// PublishApiRecord copies the ops in; ReserveApiRecord hands out room for up to max_words ops in the ring itself (null:
// no room) and CommitApiRecord publishes what was written there (one producer: refused, 0, when another record was
// published in between). The record's number (0: not published).
uint32_t PublishApiRecord(const uint32_t* ops, uint32_t n);
uint32_t* ReserveApiRecord(uint32_t max_words);
uint32_t CommitApiRecord(const uint32_t* ops, uint32_t n);

}  // namespace me::native
