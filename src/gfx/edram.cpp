#include "gfx/edram.h"

#include "rex/graphics/xenos.h"

namespace broken_bond::gfx {
namespace {

constexpr uint32_t kTileCount = rex::graphics::xenos::kEdramTileCount;

bool Span(const EdramSurface& surface, EdramSpan& out, std::string& reason) {
  if (surface.base_tiles >= kTileCount) {
    reason = "EDRAM base is outside the 2048 tiles";
    return false;
  }
  if (surface.pitch_pixels == 0 || surface.height_pixels == 0) {
    reason = "EDRAM surface has no pixels";
    return false;
  }
  if (surface.msaa > static_cast<uint32_t>(rex::graphics::xenos::MsaaSamples::k4X)) {
    reason = "EDRAM MSAA is not 1x, 2x, or 4x";
    return false;
  }
  const auto msaa = static_cast<rex::graphics::xenos::MsaaSamples>(surface.msaa);
  out.pitch_tiles = rex::graphics::xenos::GetSurfacePitchTiles(surface.pitch_pixels, msaa,
                                                               surface.is_64bpp);
  const uint32_t height_samples =
      surface.height_pixels << uint32_t(msaa >= rex::graphics::xenos::MsaaSamples::k2X);
  out.height_tiles =
      (height_samples + (rex::graphics::xenos::kEdramTileHeightSamples - 1)) /
      rex::graphics::xenos::kEdramTileHeightSamples;
  out.tile_count = out.pitch_tiles * out.height_tiles;
  if (out.pitch_tiles == 0 || out.height_tiles == 0 || out.tile_count == 0) {
    reason = "EDRAM surface covers no tiles";
    return false;
  }
  return true;
}

}  // namespace

bool EdramSpanFor(const EdramSurface& surface, EdramSpan& out, std::string& reason) {
  return Span(surface, out, reason);
}

bool EdramTilesOverlap(const EdramSurface& color, const EdramSurface& depth, bool& overlap,
                       std::string& reason) {
  EdramSpan color_span;
  EdramSpan depth_span;
  if (!Span(color, color_span, reason) || !Span(depth, depth_span, reason)) {
    return false;
  }
  bool used[kTileCount] = {};
  for (uint32_t tile = 0; tile < color_span.tile_count; ++tile) {
    used[(color.base_tiles + tile) % kTileCount] = true;
    if (tile + 1 >= kTileCount) {
      break;
    }
  }
  overlap = false;
  for (uint32_t tile = 0; tile < depth_span.tile_count; ++tile) {
    if (used[(depth.base_tiles + tile) % kTileCount]) {
      overlap = true;
      return true;
    }
    if (tile + 1 >= kTileCount) {
      break;
    }
  }
  return true;
}

}  // namespace broken_bond::gfx
