#include "web/web_audio.h"

namespace broken_bond::web {

WebAudioRing& AudioRing() {
  static WebAudioRing ring = [] {
    WebAudioRing created{};
    created.capacity_frames = kAudioCapacityFrames;
    created.sample_rate = kAudioSampleRate;
    created.channels = kAudioChannels;
    return created;
  }();
  return ring;
}

}  // namespace broken_bond::web

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>

extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t broken_bond_web_audio_ring() {
  return reinterpret_cast<uintptr_t>(&broken_bond::web::AudioRing());
}
#endif
