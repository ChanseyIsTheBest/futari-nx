// futari-nx (2026-10-19): the guest's vertical blank with Renut's renderer (futari_renut_vblank.cpp).
#pragma once

#include <cstdint>

namespace futari::renut_vblank {

// After the runtime is set up (FutariApp::OnPostSetup), with Renut's renderer chosen: the vblank thread (it raises
// vblanks once the title's device exists).
void Start();
void Stop();
// Vblanks raised so far.
uint64_t Count();

}  // namespace futari::renut_vblank
