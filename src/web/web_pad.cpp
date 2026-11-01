// The page writes keys and gamepads into the shared block. This driver turns
// them into the controller the guest polls. The key names are the PC binds.

#include "web/web_input.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <queue>
#include <string>
#include <string_view>

REXCVAR_DECLARE(std::string, keybind_a);
REXCVAR_DECLARE(std::string, keybind_b);
REXCVAR_DECLARE(std::string, keybind_x);
REXCVAR_DECLARE(std::string, keybind_y);
REXCVAR_DECLARE(std::string, keybind_left_trigger);
REXCVAR_DECLARE(std::string, keybind_right_trigger);
REXCVAR_DECLARE(std::string, keybind_left_shoulder);
REXCVAR_DECLARE(std::string, keybind_right_shoulder);
REXCVAR_DECLARE(std::string, keybind_lstick_up);
REXCVAR_DECLARE(std::string, keybind_lstick_down);
REXCVAR_DECLARE(std::string, keybind_lstick_left);
REXCVAR_DECLARE(std::string, keybind_lstick_right);
REXCVAR_DECLARE(std::string, keybind_lstick_press);
REXCVAR_DECLARE(std::string, keybind_rstick_up);
REXCVAR_DECLARE(std::string, keybind_rstick_down);
REXCVAR_DECLARE(std::string, keybind_rstick_left);
REXCVAR_DECLARE(std::string, keybind_rstick_right);
REXCVAR_DECLARE(std::string, keybind_rstick_press);
REXCVAR_DECLARE(std::string, keybind_dpad_up);
REXCVAR_DECLARE(std::string, keybind_dpad_down);
REXCVAR_DECLARE(std::string, keybind_dpad_left);
REXCVAR_DECLARE(std::string, keybind_dpad_right);
REXCVAR_DECLARE(std::string, keybind_back);
REXCVAR_DECLARE(std::string, keybind_start);
REXCVAR_DECLARE(std::string, keybind_guide);

namespace broken_bond::web {
namespace {

constexpr rex::input::DeviceId kWebPad = static_cast<rex::input::DeviceId>(0x57454200);
constexpr uint32_t kKeyDown = 1;
constexpr uint32_t kKeyUp = 2;
constexpr uint32_t kBlur = 9;
constexpr uint8_t kModShift = 1u << 0;
constexpr uint8_t kModCtrl = 1u << 1;
constexpr uint8_t kModAlt = 1u << 2;

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_B;
using rex::input::X_INPUT_GAMEPAD_BACK;
using rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
using rex::input::X_INPUT_GAMEPAD_DPAD_LEFT;
using rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT;
using rex::input::X_INPUT_GAMEPAD_DPAD_UP;
using rex::input::X_INPUT_GAMEPAD_GUIDE;
using rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_LEFT_THUMB;
using rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER;
using rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
using rex::input::X_INPUT_GAMEPAD_START;
using rex::input::X_INPUT_GAMEPAD_X;
using rex::input::X_INPUT_GAMEPAD_Y;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_KEYSTROKE_KEYDOWN;
using rex::input::X_INPUT_KEYSTROKE_KEYUP;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;
using rex::ui::VirtualKey;

uint16_t ScancodeToVirtualKey(uint32_t scancode) {
  if (scancode >= 4 && scancode <= 29) return static_cast<uint16_t>(0x41 + (scancode - 4));
  if (scancode >= 30 && scancode <= 38) return static_cast<uint16_t>(0x31 + (scancode - 30));
  switch (scancode) {
    case 39: return static_cast<uint16_t>(VirtualKey::k0);
    case 40: return static_cast<uint16_t>(VirtualKey::kReturn);
    case 41: return static_cast<uint16_t>(VirtualKey::kEscape);
    case 42: return static_cast<uint16_t>(VirtualKey::kBack);
    case 43: return static_cast<uint16_t>(VirtualKey::kTab);
    case 44: return static_cast<uint16_t>(VirtualKey::kSpace);
    case 51: return static_cast<uint16_t>(VirtualKey::kOem1);
    case 52: return static_cast<uint16_t>(VirtualKey::kOem7);
    case 79: return static_cast<uint16_t>(VirtualKey::kRight);
    case 80: return static_cast<uint16_t>(VirtualKey::kLeft);
    case 81: return static_cast<uint16_t>(VirtualKey::kDown);
    case 82: return static_cast<uint16_t>(VirtualKey::kUp);
    case 224:
    case 228: return static_cast<uint16_t>(VirtualKey::kControl);
    case 225:
    case 229: return static_cast<uint16_t>(VirtualKey::kShift);
    case 226:
    case 230: return static_cast<uint16_t>(VirtualKey::kMenu);
    default: return 0;
  }
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
  while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
  return text;
}

uint8_t TakeModifiers(std::string_view& token) {
  uint8_t mods = 0;
  for (;;) {
    const size_t plus = token.find('+');
    if (plus == std::string_view::npos || plus == 0) break;
    const std::string_view head = token.substr(0, plus);
    uint8_t bit = 0;
    if (head == "Shift") bit = kModShift;
    else if (head == "Ctrl" || head == "Control") bit = kModCtrl;
    else if (head == "Alt") bit = kModAlt;
    else break;
    mods |= bit;
    token.remove_prefix(plus + 1);
  }
  return mods;
}

bool BindPressed(const bool (&down)[256], const std::string& spec) {
  uint8_t live = 0;
  if (down[static_cast<uint16_t>(VirtualKey::kShift)]) live |= kModShift;
  if (down[static_cast<uint16_t>(VirtualKey::kControl)]) live |= kModCtrl;
  if (down[static_cast<uint16_t>(VirtualKey::kMenu)]) live |= kModAlt;
  std::string_view rest(spec);
  while (!rest.empty()) {
    const size_t comma = rest.find(',');
    std::string_view token = Trim(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
    if (token.empty() || TakeModifiers(token) != live) continue;
    const VirtualKey key = rex::ui::ParseVirtualKey(token);
    const auto index = static_cast<uint16_t>(key);
    if (key != VirtualKey::kNone && index < 256 && down[index]) return true;
  }
  return false;
}

int16_t ClampStick(float value) {
  const float scaled = std::clamp(value, -1.0f, 1.0f) * 32767.0f;
  return static_cast<int16_t>(std::lround(scaled));
}

int16_t Larger(int16_t a, int16_t b) {
  return std::abs(static_cast<int>(a)) >= std::abs(static_cast<int>(b)) ? a : b;
}

uint16_t StandardButtons(uint32_t mask) {
  uint16_t buttons = 0;
  auto take = [&](int bit, uint16_t flag) {
    if (mask & (1u << bit)) buttons = static_cast<uint16_t>(buttons | flag);
  };
  take(0, X_INPUT_GAMEPAD_A);
  take(1, X_INPUT_GAMEPAD_B);
  take(2, X_INPUT_GAMEPAD_X);
  take(3, X_INPUT_GAMEPAD_Y);
  take(4, X_INPUT_GAMEPAD_LEFT_SHOULDER);
  take(5, X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  take(8, X_INPUT_GAMEPAD_BACK);
  take(9, X_INPUT_GAMEPAD_START);
  take(10, X_INPUT_GAMEPAD_LEFT_THUMB);
  take(11, X_INPUT_GAMEPAD_RIGHT_THUMB);
  take(12, X_INPUT_GAMEPAD_DPAD_UP);
  take(13, X_INPUT_GAMEPAD_DPAD_DOWN);
  take(14, X_INPUT_GAMEPAD_DPAD_LEFT);
  take(15, X_INPUT_GAMEPAD_DPAD_RIGHT);
  return buttons;
}

class WebPadDriver final : public rex::input::InputDriver {
 public:
  explicit WebPadDriver(rex::ui::Window* window, size_t z_order) : InputDriver(window, z_order) {}

  X_STATUS Setup() override {
    REXLOG_INFO("web input: arrows move the menu, WASD walks, Enter or X starts, Space confirms");
    return X_STATUS_SUCCESS;
  }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kWebPad;
    info.name = "Keyboard and gamepad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kWebPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kWebPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    DrainKeys();
    uint16_t buttons = 0;
    if (BindPressed(down_, REXCVAR_GET(keybind_a))) buttons |= X_INPUT_GAMEPAD_A;
    if (BindPressed(down_, REXCVAR_GET(keybind_b))) buttons |= X_INPUT_GAMEPAD_B;
    if (BindPressed(down_, REXCVAR_GET(keybind_x))) buttons |= X_INPUT_GAMEPAD_X;
    if (BindPressed(down_, REXCVAR_GET(keybind_y))) buttons |= X_INPUT_GAMEPAD_Y;
    if (BindPressed(down_, REXCVAR_GET(keybind_left_shoulder))) buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
    if (BindPressed(down_, REXCVAR_GET(keybind_right_shoulder))) buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    if (BindPressed(down_, REXCVAR_GET(keybind_lstick_press))) buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_press))) buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
    if (BindPressed(down_, REXCVAR_GET(keybind_back))) buttons |= X_INPUT_GAMEPAD_BACK;
    if (BindPressed(down_, REXCVAR_GET(keybind_start))) buttons |= X_INPUT_GAMEPAD_START;
    if (BindPressed(down_, REXCVAR_GET(keybind_guide))) buttons |= X_INPUT_GAMEPAD_GUIDE;
    if (BindPressed(down_, REXCVAR_GET(keybind_dpad_up))) buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    if (BindPressed(down_, REXCVAR_GET(keybind_dpad_down))) buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (BindPressed(down_, REXCVAR_GET(keybind_dpad_left))) buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (BindPressed(down_, REXCVAR_GET(keybind_dpad_right))) buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    // The menu reads the d-pad. Plain arrows are also the camera, so they move
    // a cursor without Shift. Shift+arrows stays d-pad only.
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_up))) buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_down))) buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_left))) buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_right))) buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

    int left_x = 0;
    int left_y = 0;
    if (BindPressed(down_, REXCVAR_GET(keybind_lstick_left))) left_x -= 32767;
    if (BindPressed(down_, REXCVAR_GET(keybind_lstick_right))) left_x += 32767;
    if (BindPressed(down_, REXCVAR_GET(keybind_lstick_up))) left_y += 32767;
    if (BindPressed(down_, REXCVAR_GET(keybind_lstick_down))) left_y -= 32767;
    int right_x = 0;
    int right_y = 0;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_left))) right_x -= 32767;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_right))) right_x += 32767;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_up))) right_y += 32767;
    if (BindPressed(down_, REXCVAR_GET(keybind_rstick_down))) right_y -= 32767;
    uint8_t left_trigger = BindPressed(down_, REXCVAR_GET(keybind_left_trigger)) ? 0xFF : 0;
    uint8_t right_trigger = BindPressed(down_, REXCVAR_GET(keybind_right_trigger)) ? 0xFF : 0;

    const WebPadState pad = ReadPad();
    if (pad.connected) {
      buttons = static_cast<uint16_t>(buttons | StandardButtons(pad.buttons));
      left_trigger = std::max(left_trigger, TriggerByte(pad.triggers[0], pad.buttons & (1u << 6)));
      right_trigger = std::max(right_trigger, TriggerByte(pad.triggers[1], pad.buttons & (1u << 7)));
      left_x = Larger(static_cast<int16_t>(std::clamp(left_x, -32767, 32767)), ClampStick(pad.axes[0]));
      left_y = Larger(static_cast<int16_t>(std::clamp(left_y, -32767, 32767)), ClampStick(-pad.axes[1]));
      right_x = Larger(static_cast<int16_t>(std::clamp(right_x, -32767, 32767)), ClampStick(pad.axes[2]));
      right_y = Larger(static_cast<int16_t>(std::clamp(right_y, -32767, 32767)), ClampStick(-pad.axes[3]));
    }

    if (!logged_stick_ && (left_x != 0 || left_y != 0 || right_x != 0 || right_y != 0)) {
      logged_stick_ = true;
      REXLOG_INFO("web input: stick {}, {} and {}, {}", left_x, left_y, right_x, right_y);
    }
    NoteEdges(buttons, left_trigger, right_trigger);
    if (out_state) {
      out_state->packet_number = packet_;
      out_state->gamepad.buttons = buttons;
      out_state->gamepad.left_trigger = left_trigger;
      out_state->gamepad.right_trigger = right_trigger;
      out_state->gamepad.thumb_lx = static_cast<int16_t>(std::clamp(left_x, -32767, 32767));
      out_state->gamepad.thumb_ly = static_cast<int16_t>(std::clamp(left_y, -32767, 32767));
      out_state->gamepad.thumb_rx = static_cast<int16_t>(std::clamp(right_x, -32767, 32767));
      out_state->gamepad.thumb_ry = static_cast<int16_t>(std::clamp(right_y, -32767, 32767));
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kWebPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t, X_INPUT_KEYSTROKE* out_keystroke) override {
    if (id != kWebPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    GetDeviceState(id, nullptr);
    if (strokes_.empty()) return X_ERROR_EMPTY;
    if (out_keystroke) *out_keystroke = strokes_.front();
    strokes_.pop();
    return X_ERROR_SUCCESS;
  }

 private:
  static WebPadState ReadPad() {
    WebInputBlock& block = InputBlock();
    WebPadState pad{};
    for (int attempt = 0; attempt < 4; ++attempt) {
      const uint32_t seq = block.pad_seq.load(std::memory_order_acquire);
      if (seq & 1u) continue;
      pad = block.pads[0];
      if (block.pad_seq.load(std::memory_order_acquire) == seq) return pad;
    }
    return pad;
  }

  static uint8_t TriggerByte(float value, bool pressed) {
    if (pressed && value <= 0.0f) return 0xFF;
    return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
  }

  void DrainKeys() {
    WebInputBlock& block = InputBlock();
    const uint32_t head = block.event_head.load(std::memory_order_acquire);
    uint32_t tail = block.event_tail.load(std::memory_order_relaxed);
    uint32_t seen = 0;
    while (tail != head && seen < 256) {
      const WebInputEvent event = block.events[tail & 255u];
      ++tail;
      ++seen;
      if (event.type == kBlur) {
        std::memset(down_, 0, sizeof(down_));
        continue;
      }
      if (event.type != kKeyDown && event.type != kKeyUp) continue;
      const uint16_t key = ScancodeToVirtualKey(event.code);
      if (key < 256) down_[key] = event.type == kKeyDown;
    }
    block.event_tail.store(tail, std::memory_order_release);
  }

  void NoteEdges(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger) {
    const uint16_t changed = static_cast<uint16_t>(buttons ^ held_buttons_);
    auto edge = [&](uint16_t flag, uint16_t key) {
      if ((changed & flag) == 0 || strokes_.size() >= 64) return;
      X_INPUT_KEYSTROKE stroke{};
      stroke.virtual_key = key;
      stroke.flags = (buttons & flag) ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
      strokes_.push(stroke);
    };
    edge(X_INPUT_GAMEPAD_A, static_cast<uint16_t>(VirtualKey::kXInputPadA));
    edge(X_INPUT_GAMEPAD_B, static_cast<uint16_t>(VirtualKey::kXInputPadB));
    edge(X_INPUT_GAMEPAD_X, static_cast<uint16_t>(VirtualKey::kXInputPadX));
    edge(X_INPUT_GAMEPAD_Y, static_cast<uint16_t>(VirtualKey::kXInputPadY));
    edge(X_INPUT_GAMEPAD_LEFT_SHOULDER, static_cast<uint16_t>(VirtualKey::kXInputPadLShoulder));
    edge(X_INPUT_GAMEPAD_RIGHT_SHOULDER, static_cast<uint16_t>(VirtualKey::kXInputPadRShoulder));
    edge(X_INPUT_GAMEPAD_START, static_cast<uint16_t>(VirtualKey::kXInputPadStart));
    edge(X_INPUT_GAMEPAD_BACK, static_cast<uint16_t>(VirtualKey::kXInputPadBack));
    edge(X_INPUT_GAMEPAD_DPAD_UP, static_cast<uint16_t>(VirtualKey::kXInputPadDpadUp));
    edge(X_INPUT_GAMEPAD_DPAD_DOWN, static_cast<uint16_t>(VirtualKey::kXInputPadDpadDown));
    edge(X_INPUT_GAMEPAD_DPAD_LEFT, static_cast<uint16_t>(VirtualKey::kXInputPadDpadLeft));
    edge(X_INPUT_GAMEPAD_DPAD_RIGHT, static_cast<uint16_t>(VirtualKey::kXInputPadDpadRight));
    edge(X_INPUT_GAMEPAD_LEFT_THUMB, static_cast<uint16_t>(VirtualKey::kXInputPadLThumbPress));
    edge(X_INPUT_GAMEPAD_RIGHT_THUMB, static_cast<uint16_t>(VirtualKey::kXInputPadRThumbPress));
    const bool left_down = left_trigger >= 30;
    const bool right_down = right_trigger >= 30;
    if (left_down != held_left_ && strokes_.size() < 64) {
      X_INPUT_KEYSTROKE stroke{};
      stroke.virtual_key = static_cast<uint16_t>(VirtualKey::kXInputPadLTrigger);
      stroke.flags = left_down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
      strokes_.push(stroke);
    }
    if (right_down != held_right_ && strokes_.size() < 64) {
      X_INPUT_KEYSTROKE stroke{};
      stroke.virtual_key = static_cast<uint16_t>(VirtualKey::kXInputPadRTrigger);
      stroke.flags = right_down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
      strokes_.push(stroke);
    }
    if (buttons != held_buttons_ || left_down != held_left_ || right_down != held_right_) ++packet_;
    held_buttons_ = buttons;
    held_left_ = left_down;
    held_right_ = right_down;
  }

  bool down_[256] = {};
  bool logged_stick_ = false;
  uint16_t held_buttons_ = 0;
  bool held_left_ = false;
  bool held_right_ = false;
  uint32_t packet_ = 0;
  std::queue<X_INPUT_KEYSTROKE> strokes_;
};

}  // namespace

std::unique_ptr<rex::input::InputDriver> CreateWebPadDriver() {
  return std::make_unique<WebPadDriver>(nullptr, 0);
}

}  // namespace broken_bond::web
