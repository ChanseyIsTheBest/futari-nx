// futari-nx (2026-10-19): banjo-nx's on-screen profiler window (its src/profiler, ported from UnleashedRecomp), which
// Renut's frame statistics feed (src/renut_engine/perf/frame_stats.cpp). Futari has no such window: the rows go
// nowhere. The [renut] 10 s report and the console profiler's counters carry the same numbers.
#pragma once

namespace banjo::profiler {

inline void SetGpuFrameMs(double) {}
inline void SetRenderThreadMs(double) {}
inline void RecordHostPresent() {}

}  // namespace banjo::profiler
