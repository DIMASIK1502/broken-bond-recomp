// Checks the guest address fold and the page/wasm layouts.
// Prints the first failed check and returns 1, or prints "ok".

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "web/guest_fold.h"
#include "web/web_audio.h"
#include "web/web_input.h"

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      std::printf("web_guest_fold_test: %d: %s\n", __LINE__, #cond); \
      return 1;                                              \
    }                                                        \
  } while (0)

int main() {
  using broken_bond::web::HostOffset;
  CHECK(HostOffset(0xE0000000u) == 0x90001000);
  CHECK(HostOffset(0xE0001000u) == 0x90002000);
  CHECK(HostOffset(0x7F000000u) == 0x90000000);
  CHECK(HostOffset(0xA0000000u) == 0x90000000);
  CHECK(HostOffset(0xC0000000u) == 0x90000000);
  CHECK(HostOffset(0x90001000u) == 0x80001000);
  CHECK(HostOffset(0x80000000u) == 0x80000000);
  CHECK(sizeof(broken_bond::web::WebInputBlock) == 4256);
  CHECK(offsetof(broken_bond::web::WebAudioRing, samples) == 32);
  CHECK(offsetof(broken_bond::web::WebAudioRing, sample_rate) == 12);
  std::printf("ok\n");
  return 0;
}
