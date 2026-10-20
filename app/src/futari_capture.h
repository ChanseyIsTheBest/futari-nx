// Short video recordings of the game image for testing, taken from the presenter's guest output (what the game
// presents), so they work with the window minimized and nothing can cover them. Same design and file format as the
// banjo-nx recorder: tools/make_video.sh turns the raw file into an MP4 and a contact sheet.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace rex::ui {
class Presenter;
struct RawImage;
}

// The recorder takes one image per frame the game presents: it follows the kernel's VdSwap count
// (rex::kernel::xboxkrnl::GuestSwapCount), so it needs no hook on the game's own flip.

namespace futari::capture {

// Starts the recorder thread when futari_video_s > 0.
void Start(std::function<rex::ui::Presenter*()> get_presenter);

// The same for a renderer without the SDK presenter (Renut's): `grab` takes the presented image, `frames` counts the
// game's Swaps (rex::latency::SwapCount).
void Start(std::function<bool(rex::ui::RawImage&)> grab, std::function<uint64_t()> frames);

// Stops it (writes what it has, if anything).
void Stop();

}  // namespace futari::capture
