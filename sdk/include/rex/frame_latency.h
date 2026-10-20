/**
 * Frame latency probe: how long a frame takes from the moment the game reads its controller to the moment the
 * display can show it. Four moments per frame, all on one steady clock:
 *
 *   input     the game's first controller read after its previous Swap (XamInputGetState and friends)
 *   swap      the game's VdSwap that ends the frame
 *   present   the renderer handed that Swap's image to the display (after vkQueuePresentKHR)
 *   vsync     the first vertical blank of the display after the present (only where the platform reports the
 *             display's own vsync: the Switch's vi vsync event). The image is scanned out from there.
 *
 * Every function is cheap (a few relaxed atomics) and safe from any thread. Report() gives one line of means and
 * maxima since the last report, for the 10 s report of a renderer.
 */
#pragma once

#include <cstdint>
#include <string>

namespace rex::latency {

// The game read a controller. Only the first read after a Swap counts (the frame's input).
void NoteInputRead();
// The game's VdSwap: the frame that began with the last NoteInputRead is complete on the CPU side.
void NoteGuestSwap();
// The renderer presented the image of the guest's `swap`-th Swap (1-based, counted the same way as the VdSwaps).
void NotePresented(uint64_t swap);
// The GPU has finished the image of the guest's `swap`-th Swap (seen now): the report's "input -> GPU done", the
// earliest the display can show the frame (the present above is when it was handed over). banjo-nx 2026-10-20.
void NoteGpuDone(uint64_t swap);
// A vertical blank of the display itself (not the guest's timer). image_ready: whether the GPU had finished the last
// presented image at this vsync (1), had not (0: it is shown at a later one), or the port cannot tell (-1).
void NoteDisplayVsync(int image_ready = -1);
// The port's guest vblank came without a Swap since the previous one: the game missed a frame.
void NoteVblankWithoutSwap();
// How many images were presented so far (NotePresented calls).
uint64_t PresentedCount();
// The game's Swaps so far (NoteGuestSwap calls: the VdSwaps, or the Swaps of a renderer that replaces Direct3D's and
// calls no VdSwap, such as Renut's; futari-nx 2026-10-19, from banjo-nx's SDK).
uint64_t SwapCount();
// The time (std::chrono::steady_clock nanoseconds) of the first controller read of the frame the `swap`-th Swap ends;
// 0 when not known (banjo-nx's input -> display measurement).
uint64_t InputTimeOf(uint64_t swap);
// One line: "input -> swap a ms, swap -> present b ms, present -> display vsync c ms (input -> vsync d ms); n
// frames, m presented before the next vsync superseded an earlier one". Empty when no frame completed.
std::string Report();

// A trace of the order of events over a few frames (a diagnostic): StartTrace(n) records every event of the next n
// guest Swaps (the four above, every controller read, and what the port adds with TraceEvent: the guest's vblank,
// the game thread's waits...), then TakeTrace() returns them as text, one line per event with its time in ms from
// the first, once (empty before the trace is complete).
void StartTrace(uint32_t swaps);
void TraceEvent(const char* what);
std::string TakeTrace();

}  // namespace rex::latency
