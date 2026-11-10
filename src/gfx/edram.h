// Tile span of one EDRAM color or depth surface. The sample bytes inside a
// tile stay in the SDK; this only answers which of the 2048 tiles a surface
// owns, including the wrap past the last tile.

#pragma once

#include <cstdint>
#include <string>

namespace broken_bond::gfx {

struct EdramSurface {
  uint32_t base_tiles = 0;
  uint32_t pitch_pixels = 0;
  uint32_t height_pixels = 0;
  // Xenos MsaaSamples: 0 is 1x, 1 is 2x, 2 is 4x.
  uint32_t msaa = 0;
  bool is_64bpp = false;
};

struct EdramSpan {
  uint32_t pitch_tiles = 0;
  uint32_t height_tiles = 0;
  uint32_t tile_count = 0;
};

bool EdramSpanFor(const EdramSurface& surface, EdramSpan& out, std::string& reason);

// overlap is true when any tile of the two surfaces is the same tile after the
// 2048 wrap. A full 2048-tile surface overlaps every other surface.
bool EdramTilesOverlap(const EdramSurface& color, const EdramSurface& depth, bool& overlap,
                       std::string& reason);

}  // namespace broken_bond::gfx
