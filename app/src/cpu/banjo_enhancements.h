// futari-nx (2026-10-19): what banjo-nx's renderer code (app/src/renut_engine/gpu, Renut's renderer as banjo-nx adapted
// it) asks of banjo-nx's enhancement switches, answered by Futari's (src/futari_enhancements.h): an enhancement is on
// when its own key is and its group's master (futari_cpu_enhancements / futari_noncpu_enhancements) is.
// src/renut_engine/futari_renut_shim.cpp.
#pragma once

namespace banjo {

enum class EnhancementGroup { kCpu, kNonCpu };

// Own key AND the group's master switch.
bool EnhancementActive(EnhancementGroup group, bool own_key);

// The generated code's self-checks, reported with the renderer's 10 s report (Futari's
// futari::native::CodegenChecksReport).
void CodegenChecksReport(int seconds);

}  // namespace banjo
