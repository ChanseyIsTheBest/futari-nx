#pragma once

// Periodic "[mem]" log lines (cvar futari_memory_log_s); see futari_memory_log.cpp.
namespace futari::memory_log {

void Start();
void Stop();

}  // namespace futari::memory_log
