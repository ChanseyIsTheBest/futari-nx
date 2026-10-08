#pragma once

// PC sampling profiler (cvar futari_pc_profile_s); see futari_pc_profiler.cpp. Does nothing on the Switch, where the
// SDK's sampler (sdk/src/ui/switch_perf.cpp) does the same job.
namespace futari::pc_profiler {

void Start();
void Stop();

}  // namespace futari::pc_profiler
