// Native renderer of Broken Bond on Plume (docs/HOST_RENDERER.md, phase 4).
//
// Owns the Vulkan device and the swapchain on the game window. The game runs
// on rexgpu-host (--gpu_plugin=host), so nothing else draws into that window.
// Each guest draw is translated from the D3D device shadow (docs/D3D_MAP.md)
// when the title issues it; Present submits the frame.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace broken_bond::render {

struct GuestDraw {
  bool indexed = false;
  uint32_t device = 0;
  uint32_t prim = 0;         // xenos::PrimitiveType, as D3D passes it
  uint32_t base_vertex = 0;  // indexed only
  uint32_t start = 0;        // first index, or first vertex
  uint32_t count = 0;        // index count, or vertex count
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  // native_window is the HWND (Windows) or ANativeWindow* (Android).
  bool Open(void* native_window, std::string& reason);

  // One guest draw, from the guest thread, before the original runs.
  void Draw(const GuestDraw& draw, const uint8_t* base);

  // One guest present. Called from the guest thread that runs Swap.
  void Present();

  // Counts since the last call, for the 5 s log.
  std::string TakeStats();

  const std::string& device_name() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace broken_bond::render
