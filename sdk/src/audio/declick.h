/**
 * @file        audio/declick.h
 * @brief       Smoothing of the steps a title makes on its 256-sample frame edges (Switch output stage)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * futari-nx (2026-10-08). The game's own audio code starts and stops sounds on 256-sample frame edges without a fade.
 * Where a sound's waveform is far from zero there, the output steps (0.1-0.4 of full scale) and clicks; in Xbox 360
 * mode a sound retriggered every 4 game frames cuts its previous instance each time, 15 steps a second: crackling
 * (the user's slot 5 replay on the PC: 37 steps in 10 s). The steps are in what the game delivers, the same with the
 * native audio functions on and off, so they are smoothed at the output: the jump is removed from the first samples of
 * the new frame and comes back gradually, and the sound is unchanged everywhere else.
 *
 * Portable (no libnx): the Switch driver uses it, and tools/declick_test.cpp runs the same code on a PC audio dump.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rex::audio {

struct DeclickOptions {
  // audio_switch_declick_smooth: true = the jump comes back over the whole frame (256 samples, 5.3 ms) on a
  // raised-cosine curve, which leaves no audible tick; false = linearly over 64 samples (1.3 ms), the first version.
  bool smooth = true;
  // audio_switch_declick_predict: also catch a step against where the waveform was heading (the last two samples'
  // slope), measured against the frames' typical sample-to-sample curvature. A step hidden in busy sound is missed by
  // the first rule (larger than 1.5x the largest jump inside both frames), which a dense mix rarely satisfies.
  bool predict = true;
};

struct DeclickState {
  bool valid = false;
  float last[2] = {};      // last sample of the previous frame
  float before[2] = {};    // the one before it
  float inner = 0.0f;      // largest sample-to-sample jump inside the previous frame
  float curvature = 0.0f;  // mean |second difference| inside the previous frame
};

// One frame of interleaved stereo (frame_samples per channel). Returns true when its leading edge was smoothed.
inline bool DeclickFrame(float* f, uint32_t frame_samples, DeclickState& s, const DeclickOptions& o) {
  float inner = 0.0f;
  float curvature = 0.0f;
  for (uint32_t i = 1; i < frame_samples; ++i) {
    inner = std::max(inner, std::max(std::fabs(f[i * 2] - f[i * 2 - 2]), std::fabs(f[i * 2 + 1] - f[i * 2 - 1])));
  }
  if (o.predict) {
    for (uint32_t i = 1; i + 1 < frame_samples; ++i) {
      curvature += std::fabs(f[i * 2 + 2] - 2.0f * f[i * 2] + f[i * 2 - 2]) +
                   std::fabs(f[i * 2 + 3] - 2.0f * f[i * 2 + 1] + f[i * 2 - 1]);
    }
    curvature /= float(2 * (frame_samples - 2));
  }
  bool smoothed = false;
  if (s.valid) {
    const float step[2] = {f[0] - s.last[0], f[1] - s.last[1]};
    const float jump = std::max(std::fabs(step[0]), std::fabs(step[1]));
    // The first rule: a step clearly larger than any jump inside the two frames (audio_pop_detect's rule).
    bool hit = std::isfinite(jump) && jump > 0.05f && jump > 1.5f * std::max(inner, s.inner);
    float remove[2] = {step[0], step[1]};
    if (o.predict && std::isfinite(jump)) {
      // Against the slope: the deviation of the first sample from the straight continuation of the last two.
      const float error[2] = {f[0] - (2.0f * s.last[0] - s.before[0]), f[1] - (2.0f * s.last[1] - s.before[1])};
      const float deviation = std::max(std::fabs(error[0]), std::fabs(error[1]));
      if (deviation > 0.03f && jump > 0.03f &&
          deviation > 6.0f * std::max(std::max(curvature, s.curvature), 1e-4f)) {
        hit = true;
      }
    }
    if (hit) {
      if (o.smooth) {
        // 1 -> 0 over the whole frame: 0.5 (1 + cos(pi i / n)).
        const float w = 3.14159265f / float(frame_samples);
        for (uint32_t i = 0; i < frame_samples; ++i) {
          const float keep = 0.5f * (1.0f + std::cos(w * float(i)));
          f[i * 2] -= remove[0] * keep;
          f[i * 2 + 1] -= remove[1] * keep;
        }
      } else {
        constexpr uint32_t kLinear = 64;  // 1.33 ms at 48 kHz
        for (uint32_t i = 0; i < kLinear && i < frame_samples; ++i) {
          const float keep = 1.0f - float(i) / float(kLinear);
          f[i * 2] -= remove[0] * keep;
          f[i * 2 + 1] -= remove[1] * keep;
        }
      }
      smoothed = true;
    }
  }
  s.last[0] = f[(frame_samples - 1) * 2];
  s.last[1] = f[(frame_samples - 1) * 2 + 1];
  s.before[0] = f[(frame_samples - 2) * 2];
  s.before[1] = f[(frame_samples - 2) * 2 + 1];
  s.inner = inner;
  s.curvature = curvature;
  s.valid = true;
  return smoothed;
}

}  // namespace rex::audio
