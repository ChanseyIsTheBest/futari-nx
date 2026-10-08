// Runs the Switch driver's declick (sdk/src/audio/declick.h, the same code) on a PC audio dump (audio_dump_s: the
// game's 6-channel float frames before the driver), folded to stereo as the driver does, and writes a window of the
// result as a 16-bit stereo WAV to listen to. Counts the frame edges each setting smooths.
//
//   clang++ -O2 -std=c++20 tools/declick_test.cpp -o out/declick_test.exe   (after . tools\win\env.ps1)
//   out/declick_test.exe <audio_dump_0.wav> <out.wav> <from_s> <seconds> <mode: off|v1|smooth|predict|both>
//
// v1 = the first version (linear 64 samples, step rule only); smooth / predict = one of the two 2026-10-08 keys;
// both = the two keys on (the default). Times are seconds into the dump.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../sdk/src/audio/declick.h"

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr, "usage: declick_test <dump.wav> <out.wav> <from_s> <seconds> <off|v1|smooth|predict|both>\n");
    return 2;
  }
  FILE* in = std::fopen(argv[1], "rb");
  if (!in) return 1;
  std::vector<uint8_t> head(64);
  std::fread(head.data(), 1, 64, in);
  size_t data_at = 0;
  for (size_t i = 12; i + 8 <= 64; ++i) {
    if (std::memcmp(&head[i], "data", 4) == 0) { data_at = i + 8; break; }
  }
  const uint16_t channels = uint16_t(head[22] | head[23] << 8);
  if (!data_at || channels != 6) { std::fprintf(stderr, "not a 6-channel dump\n"); return 1; }
  std::fseek(in, 0, SEEK_END);
  const long long bytes = std::ftell(in) - (long long)data_at;
  const size_t frames_total = size_t(bytes / (6 * 4)) / 256;
  const std::string mode = argv[5];
  rex::audio::DeclickOptions options;
  options.smooth = mode == "smooth" || mode == "both";
  options.predict = mode == "predict" || mode == "both";
  const bool on = mode != "off";
  const size_t from = size_t(std::atof(argv[3]) * 48000) / 256;
  const size_t count = size_t(std::atof(argv[4]) * 48000) / 256;
  rex::audio::DeclickState state;
  std::vector<float> raw(256 * 6), frame(256 * 2);
  std::vector<int16_t> out;
  size_t smoothed = 0, smoothed_window = 0;
  float peak = 0.0f;
  std::fseek(in, long(data_at), SEEK_SET);
  for (size_t fi = 0; fi < frames_total; ++fi) {
    if (std::fread(raw.data(), sizeof(float), raw.size(), in) != raw.size()) break;
    // The driver's fold (sdk/include/rex/audio/downmix.h defaults, gain 1): FL FR C LFE SL SR.
    for (int i = 0; i < 256; ++i) {
      const float* s = &raw[i * 6];
      frame[i * 2] = 0.58578644f * (s[0] + 0.70710678f * s[2] + 0.70710678f * s[4]);
      frame[i * 2 + 1] = 0.58578644f * (s[1] + 0.70710678f * s[2] + 0.70710678f * s[5]);
    }
    if (on && rex::audio::DeclickFrame(frame.data(), 256, state, options)) {
      ++smoothed;
      if (fi >= from && fi < from + count) ++smoothed_window;
    }
    if (fi >= from && fi < from + count) {
      for (float v : frame) {
        peak = std::max(peak, std::fabs(v));
        out.push_back(int16_t(std::lrint(std::clamp(v, -1.0f, 1.0f) * 32767.0f)));
      }
    }
  }
  std::fclose(in);
  FILE* o = std::fopen(argv[2], "wb");
  if (!o) return 1;
  const uint32_t n = uint32_t(out.size() * 2);
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, o); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, o); };
  std::fwrite("RIFF", 1, 4, o); u32(36 + n); std::fwrite("WAVEfmt ", 1, 8, o); u32(16); u16(1); u16(2); u32(48000);
  u32(48000 * 4); u16(4); u16(16); std::fwrite("data", 1, 4, o); u32(n);
  std::fwrite(out.data(), 2, out.size(), o);
  std::fclose(o);
  std::printf("%s: %zu frames (%.0f s), %zu edges smoothed (%.2f/s); window: %zu smoothed, peak %.3f\n", mode.c_str(),
              frames_total, frames_total * 256 / 48000.0, smoothed, smoothed / (frames_total * 256 / 48000.0),
              smoothed_window, peak);
  return 0;
}
