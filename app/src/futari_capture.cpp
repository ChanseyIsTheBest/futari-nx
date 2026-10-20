// Short video recordings of the game image (see futari_capture.h).
//
// From futari_video_s seconds after start, futari_video_frames consecutive presented frames are written as they come to
// captures/video_<s>s.rgba next to the executable (raw R8G8B8X8, frames back to back; streamed, so a long recording
// does not need RAM, ~55 MB per second at half resolution on disk) with a .txt beside it (width, height, measured fps,
// frame count). tools/make_video.sh turns it into an MP4 with ffmpeg.

#include "futari_capture.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/ui/presenter.h>

#define XXH_INLINE_ALL  // as the renderer does: the streaming API is not in a linked library
#include <xxhash.h>

REXCVAR_DEFINE_INT32(futari_video_s, 0, "Futari",
                     "Record a short video of the game image this many seconds after start, to captures/ next to "
                     "the executable (0 = off; works minimized). tools/make_video.sh makes the MP4")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(futari_video_frames, 300, "Futari", "Frames in the video (futari_video_s); the game runs at 60 fps")
    .range(1, 36000);
REXCVAR_DEFINE_BOOL(futari_video_half, true, "Futari",
                    "Record the video at half resolution (~55 MB of disk per second instead of ~220 MB)");
// Lockstep comparisons (sa2-nx's design): with vblank_per_swap the game advances one frame per swap whatever the PC's
// speed, so two runs show the same image at the same game frame. tools/compare_frames.py compares two .frames files.
REXCVAR_DEFINE_INT32(futari_video_frame, 0, "Futari",
                     "After futari_video_s, also wait for this game frame (VdSwap count) before recording (0 = off). "
                     "With vblank_per_swap every run then records the same stretch of the game")
    .range(0, 2000000000);
REXCVAR_DEFINE_INT32(futari_video_every, 1, "Futari", "Record one game frame in this many")
    .range(1, 600);
REXCVAR_DEFINE_BOOL(futari_video_hashes, false, "Futari",
                    "Write no image data: captures/<name>.frames gets one line per recorded frame, '<game frame> "
                    "<XXH3 of the full-resolution image>' (pixel comparisons of long runs without filling the disk)");

namespace futari::capture {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
std::thread g_thread;

bool StopRequested() {
  std::lock_guard lock(g_mutex);
  return g_stop;
}

void Record(std::function<bool(rex::ui::RawImage&)> grab, std::function<uint64_t()> frames, int start_s,
            int frame_count) {
  {
    std::unique_lock lock(g_mutex);
    if (g_cv.wait_for(lock, std::chrono::seconds(start_s), [] { return g_stop; })) {
      return;
    }
  }
  const uint32_t step = REXCVAR_GET(futari_video_half) ? 2 : 1;
  const auto folder = rex::filesystem::GetExecutableFolder() / "captures";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::string name = "video_" + std::to_string(start_s) + "s";
  const bool hashes = REXCVAR_GET(futari_video_hashes);
  const uint64_t first_frame = uint64_t(REXCVAR_GET(futari_video_frame));
  const uint64_t every = uint64_t(REXCVAR_GET(futari_video_every));
  std::ofstream labels;
  if (hashes) labels.open(folder / (name + ".frames"), std::ios::binary | std::ios::trunc);
  std::ofstream raw;
  if (!hashes) raw.open(folder / (name + ".rgba"), std::ios::binary | std::ios::trunc);
  if (hashes ? !labels : !raw) {
    REXLOG_WARN("[capture] cannot write {}", rex::path_to_utf8(folder / (name + ".rgba")));
    return;
  }
  std::vector<uint8_t> frame;
  int written = 0;
  uint64_t skipped_flips = 0;  // flips that came and went while a capture was still being taken
  uint32_t width = 0, height = 0;
  const auto started = std::chrono::steady_clock::now();
  uint64_t last = frames();
  REXLOG_INFO("[capture] recording {} frames from {} s", frame_count, start_s);
  while (written < frame_count && !StopRequested()) {
    // One capture per game frame, a few ms after the flip so the presenter holds the new image.
    const uint64_t now = frames();
    if (now == last || now < first_frame || (now - first_frame) % every != 0) {
      if (now != last && written) skipped_flips += now - last - 1;
      if (now != last) last = now;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (written) {
      skipped_flips += now - last - 1;
    }
    last = now;
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    rex::ui::RawImage image;
    if (!grab(image) || !image.width || !image.height ||
        (width && (image.width != width || image.height != height))) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    width = image.width;
    height = image.height;
    if (hashes) {  // the full image, rows without the stride's padding
      XXH3_state_t* state = XXH3_createState();
      XXH3_64bits_reset(state);
      for (uint32_t y = 0; y < height; ++y)
        XXH3_64bits_update(state, image.data.data() + size_t(y) * image.stride, size_t(width) * 4);
      labels << now << ' ' << std::hex << XXH3_64bits_digest(state) << std::dec << '\n';
      XXH3_freeState(state);
      if (++written % 600 == 0) labels.flush();
      continue;
    }
    const uint32_t w = width / step, h = height / step;
    frame.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
      const uint8_t* row = image.data.data() + size_t(y) * step * image.stride;
      uint8_t* out = frame.data() + size_t(y) * w * 4;
      if (step == 1) {
        std::memcpy(out, row, size_t(w) * 4);
      } else {
        for (uint32_t x = 0; x < w; ++x) {
          std::memcpy(out + size_t(x) * 4, row + size_t(x) * step * 4, 4);
        }
      }
    }
    raw.write(reinterpret_cast<const char*>(frame.data()), std::streamsize(frame.size()));
    ++written;
    if (written % 600 == 0) {
      REXLOG_INFO("[capture] {} / {} frames", written, frame_count);
    }
  }
  if (hashes) {
    labels.close();
    REXLOG_INFO("[capture] {} frame hashes from game frame {} -> {}", written, first_frame,
                rex::path_to_utf8(folder / (name + ".frames")));
    return;
  }
  raw.close();
  if (!written) {
    REXLOG_WARN("[capture] no frame recorded (no presenter or no guest output)");
    return;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const double fps = seconds > 0 ? double(written) / seconds : 60.0;
  std::ofstream(folder / (name + ".txt"), std::ios::binary)
      << width / step << ' ' << height / step << ' ' << fps << ' ' << written << '\n';
  REXLOG_INFO("[capture] video {}: {} frames of {}x{} in {:.2f} s ({:.1f} fps, {} flips skipped by the recorder) -> {}",
              name, written, width / step, height / step, seconds, fps, skipped_flips,
              rex::path_to_utf8(folder / (name + ".rgba")));
}

}  // namespace

void Start(std::function<rex::ui::Presenter*()> get_presenter) {
  if (REXCVAR_GET(futari_video_s) <= 0 || g_thread.joinable()) {
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_stop = false;
  }
  auto grab = [get_presenter = std::move(get_presenter)](rex::ui::RawImage& image) {
    rex::ui::Presenter* presenter = get_presenter ? get_presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  };
  g_thread = std::thread(Record, std::move(grab), [] { return rex::kernel::xboxkrnl::GuestSwapCount(); },
                         int(REXCVAR_GET(futari_video_s)), int(REXCVAR_GET(futari_video_frames)));
}

void Start(std::function<bool(rex::ui::RawImage&)> grab, std::function<uint64_t()> frames) {
  if (REXCVAR_GET(futari_video_s) <= 0 || g_thread.joinable()) {
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    g_stop = false;
  }
  g_thread = std::thread(Record, std::move(grab), std::move(frames), int(REXCVAR_GET(futari_video_s)),
                         int(REXCVAR_GET(futari_video_frames)));
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

}  // namespace futari::capture
