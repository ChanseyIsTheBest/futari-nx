/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>

namespace rex::runtime {
class ExportResolver;
}

namespace rex::system {
class KernelState;
}

namespace rex::kernel::xboxkrnl {

void VdQueryVideoMode(system::X_VIDEO_MODE* video_mode);

// Register video variable exports (VdGlobalDevice, VdHSIOCalibrationLock, etc.)
// Must be called during kernel initialization before XEX modules are loaded.
void RegisterVideoExports(rex::runtime::ExportResolver* export_resolver,
                          system::KernelState* kernel_state);

// Number of VdSwap calls so far: one per frame the game presents, whatever its own Direct3D looks like. For tools
// that follow the game's frames (video recorders, frame pacing logs) without a game-specific hook on its flip.
uint64_t GuestSwapCount();

// vblank_per_swap (deterministic runs): the vblank thread raises one vblank per guest swap instead of one per 1/60 s.
bool VblankPerSwap();
// For VblankPerSwap(): waits until the guest swapped since `last_swap` (updated) or the timeout passed (a guest that
// waits for vblanks without swapping, e.g. a loading screen); false when `running` went false.
bool WaitSwapForVblank(uint64_t& last_swap, const std::atomic<bool>& running);

}  // namespace rex::kernel::xboxkrnl
