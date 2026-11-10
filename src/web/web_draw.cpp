#include "web/web_draw.h"

#include "host/d3d_census.h"
#include "host/shadow_draw.h"
#include "render/renderer.h"
#include "web/web_picture.h"

#include <iostream>
#include <memory>
#include <string>

#include <rex/hook.h>

namespace broken_bond::host {

int CensusEnter(uint32_t, const PPCContext&, uint8_t*) { return -1; }
void CensusReturn(int, const PPCContext&) {}

}  // namespace broken_bond::host

namespace broken_bond::web {

std::unique_ptr<broken_bond::render::Renderer> g_renderer;

broken_bond::render::Renderer* Renderer() { return g_renderer.get(); }

bool OpenRenderer() {
  // The blue present's device stays. Destroying it from this worker calls a
  // null device-thread function. The renderer configures #canvas itself.
  broken_bond::host::ShadowKeepShaderKeys();
  auto native = std::make_unique<broken_bond::render::Renderer>();
  std::string reason;
  if (!native->Open(const_cast<char*>("#canvas"), reason)) {
    std::cout << "broken_bond web: renderer did not open: " << reason << std::endl;
    return false;
  }
  std::cout << "broken_bond web: renderer " << native->device_name() << std::endl;
  g_renderer = std::move(native);
  return true;
}

bool PresentRenderer(uint32_t frontbuffer, const uint8_t* base) {
  if (!g_renderer) return false;
  g_renderer->Present(frontbuffer, base);
  return true;
}

}  // namespace broken_bond::web

extern "C" void __imp__sub_821AE5D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AE9C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821ACB10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AD328(PPCContext& __restrict ctx, uint8_t* base);

REX_HOOK_RAW(sub_821AE5D0) {
  if (auto* renderer = broken_bond::web::Renderer()) {
    broken_bond::render::GuestDraw draw;
    draw.device = ctx.r3.u32;
    draw.prim = ctx.r4.u32;
    draw.start = ctx.r5.u32;
    draw.count = ctx.r6.u32;
    renderer->Draw(draw, base);
  }
  __imp__sub_821AE5D0(ctx, base);
}

REX_HOOK_RAW(sub_821AE9C0) {
  if (auto* renderer = broken_bond::web::Renderer()) {
    broken_bond::render::GuestDraw draw;
    draw.indexed = true;
    draw.device = ctx.r3.u32;
    draw.prim = ctx.r4.u32;
    draw.base_vertex = ctx.r5.u32;
    draw.start = ctx.r6.u32;
    draw.count = ctx.r7.u32;
    renderer->Draw(draw, base);
  }
  __imp__sub_821AE9C0(ctx, base);
}

REX_HOOK_RAW(sub_821ACB10) {
  if (auto* renderer = broken_bond::web::Renderer()) {
    renderer->Clear(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, float(ctx.f1.f64), ctx.r7.u32, base);
  }
  __imp__sub_821ACB10(ctx, base);
}

REX_HOOK_RAW(sub_821AD328) {
  if (auto* renderer = broken_bond::web::Renderer()) {
    renderer->Resolve(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r10.u32, float(ctx.f1.f64),
                      base);
  }
  __imp__sub_821AD328(ctx, base);
}
