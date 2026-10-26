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

  // D3DDevice_ClearF (821ACB10): flags (targets 0-3, 0x10 Z, 0x20 stencil),
  // rect (D3DRECT* or 0), color (float4* or 0), z, stencil.
  void Clear(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, float z, uint32_t stencil,
             const uint8_t* base);

  // D3DDevice_Resolve (821AD328): flags (low 3 bits the source, 0x100 and
  // 0x200 clear the target and depth afterwards), source rect, destination
  // texture object, destination point, clear color.
  void Resolve(uint32_t device, uint32_t flags, uint32_t rect, uint32_t texture, uint32_t point,
               uint32_t clear_color, float z, const uint8_t* base);

  // One guest present of the frontbuffer texture object Swap was given.
  // Called from the guest thread that runs Swap.
  void Present(uint32_t frontbuffer, const uint8_t* base);

  // Counts since the last call, for the 5 s log.
  std::string TakeStats();

  const std::string& device_name() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace broken_bond::render
