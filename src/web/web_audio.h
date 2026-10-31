// Playback ring shared with web/audio_worklet.js.
//
// Header, 8 uint32 words, then interleaved stereo float frames:
//   0 write_frame  4 read_frame  8 capacity_frames  12 sample_rate  16 channels
//   20 underruns   24 heartbeat  28 peak_milli      32 samples
// The page's worklet consumes frames. The SDK audio driver will produce them;
// until that driver is linked this ring stays silent and still lets the
// worklet start.

#pragma once

#include <cstddef>
#include <cstdint>

namespace broken_bond::web {

inline constexpr uint32_t kAudioCapacityFrames = 8192;
inline constexpr uint32_t kAudioSampleRate = 48000;
inline constexpr uint32_t kAudioChannels = 2;

struct WebAudioRing {
  uint32_t write_frame;
  uint32_t read_frame;
  uint32_t capacity_frames;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t underruns;
  uint32_t heartbeat;
  uint32_t peak_milli;
  float samples[kAudioCapacityFrames * kAudioChannels];
};

static_assert(offsetof(WebAudioRing, samples) == 32);
static_assert(offsetof(WebAudioRing, sample_rate) == 12);

WebAudioRing& AudioRing();

}  // namespace broken_bond::web
