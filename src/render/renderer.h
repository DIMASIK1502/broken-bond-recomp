// Native renderer of Broken Bond on Plume (docs/HOST_RENDERER.md, phase 4).
//
// Owns the Vulkan device and the swapchain on the game window. The game runs
// on rexgpu-host (--gpu_plugin=host), so nothing else draws into that window.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace broken_bond::render {

class Renderer {
 public:
  Renderer();
  ~Renderer();

  // native_window is the HWND (Windows) or ANativeWindow* (Android).
  bool Open(void* native_window, std::string& reason);

  // One guest present. Called from the guest thread that runs Swap.
  void Present();

  const std::string& device_name() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace broken_bond::render
