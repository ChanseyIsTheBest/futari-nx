// futari-nx: the Direct3D census (cvar futari_diag_d3d_census, a diagnostic): which of the game's Direct3D functions
// are called, how often, and which PM4 packets each outermost call writes. sa2-nx's census (its sa2_d3d_api.cpp and
// docs/api-renderer.md), the first step of the native Direct3D layer: it names the functions that still write PM4.
//
// The hooks are generated for a census build only (tools/d3d_census_build.py: out/census/, configured with
// -DFUTARI_CENSUS_DIR=out/census), every function of the Direct3D range of default.xex and of each game core. The
// normal build has only the draw hooks' calls, which check the cvar and return.
#pragma once

#include <cstdint>

namespace futari::d3d {

bool CensusOn();
// An outermost call decodes the PM4 between the device's write pointer (+48) before and after it. `module` names the
// module of `address` ("default" or a core's name).
void CensusEnter(uint32_t address, const uint8_t* base, const char* module);
void CensusLeave(const uint8_t* base);
// Around the command buffer's space function: the old segment is decoded up to its write pointer, then the new one is
// followed from its start.
void CensusSegmentBefore(const uint8_t* base);
void CensusSegmentAfter(const uint8_t* base);
// The draw hooks: the device (r3) and the drawing thread (only its calls decode PM4: the range holds a few functions
// other threads call, which would otherwise be charged with the drawing thread's packets), and the device's flag bytes
// at a draw (the draw-time branches the native code has to cover).
void NoteDraw(const uint8_t* base, uint32_t device);

}  // namespace futari::d3d

// The game cores reach the executable's census through its exports FutariCensusOn / FutariCensusEnter /
// FutariCensusLeave / FutariCensusSegment / FutariCensusNoteDraw (futari_d3d_census.cpp; futari_native_core.cpp).
