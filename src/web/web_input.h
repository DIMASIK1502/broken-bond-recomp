// Shared-memory input block. web/input.js writes it; the guest thread reads it.
// Offsets match that file (pad seq at 0, pads at 8, events at 144, canvas at 4240).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace broken_bond::web {

struct WebPadState {
  uint32_t connected;
  uint32_t buttons;
  float axes[4];
  float triggers[2];
};

struct WebInputEvent {
  uint32_t type;
  uint32_t code;
  int32_t x;
  int32_t y;
};

struct WebInputBlock {
  std::atomic<uint32_t> pad_seq;
  uint32_t pad_count;
  WebPadState pads[4];
  std::atomic<uint32_t> event_head;
  std::atomic<uint32_t> event_tail;
  WebInputEvent events[256];
  uint32_t canvas_width;
  uint32_t canvas_height;
  float device_pixel_ratio;
  uint32_t visible;
};

static_assert(sizeof(WebPadState) == 32);
static_assert(sizeof(WebInputEvent) == 16);
static_assert(sizeof(WebInputBlock) == 4256);
static_assert(offsetof(WebInputBlock, pads) == 8);
static_assert(offsetof(WebInputBlock, event_head) == 136);
static_assert(offsetof(WebInputBlock, events) == 144);
static_assert(offsetof(WebInputBlock, canvas_width) == 4240);

// The block the page maps. Safe to call before main.
WebInputBlock& InputBlock();

}  // namespace broken_bond::web

namespace rex::input {
class InputDriver;
}

namespace broken_bond::web {

// Keyboard and gamepad, on the same binds as the PC build.
std::unique_ptr<::rex::input::InputDriver> CreateWebPadDriver();

}  // namespace broken_bond::web
