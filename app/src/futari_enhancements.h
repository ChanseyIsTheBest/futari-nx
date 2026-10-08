#pragma once

// Every performance enhancement of the port, CPU and non-CPU, with an on/off key each and two group masters
// (futari_cpu_enhancements, futari_noncpu_enhancements). The table is in futari_enhancements.cpp: an enhancement is
// added there when it is written, so this file and its log line are the complete list.
namespace futari::enhancements {

enum class Group { kCpu, kNonCpu };

// At startup, after the config is read and before anything reads the keys (FutariApp::OnPostInitLogging): a group
// whose master is off turns all of its keys off, then one log line lists every enhancement and whether it is active.
void Apply();

// The group master (for enhancements read through their own key this is already folded in by Apply()).
bool GroupOn(Group group);

}  // namespace futari::enhancements
