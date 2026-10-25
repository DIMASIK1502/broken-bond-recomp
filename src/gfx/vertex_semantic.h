// Semantic of each vfetch in a shader from this dump. The microcode itself
// stores format 0 and stride 0. Xbox 360 patches those fields from the bound
// vertex declaration at draw time. This table says which instruction receives
// which declaration element.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace broken_bond::gfx {

struct VertexFetchSemantic {
  uint32_t instruction = 0;
  uint32_t usage = 0;
  uint32_t usage_index = 0;
  uint32_t variant = 0;
};

// One element of the vertex declaration the game binds at draw time. format is
// a Xenos VertexFormat. offset_words and stride_words are in dwords.
struct VertexElement {
  uint32_t usage = 0;
  uint32_t usage_index = 0;
  uint32_t format = 0;
  int32_t offset_words = 0;
  uint32_t stride_words = 0;
  bool signed_components = false;
  bool integer_components = false;
};

const char* VertexUsageName(uint32_t usage);

// Reads one inflated vertex shader object. Fails when the table after the
// 0x290 marker does not name exactly the vfetch instructions in the microcode.
bool ReadVertexFetchSemantics(const uint8_t* object, size_t size,
                              std::vector<VertexFetchSemantic>& out, std::string& reason);

// Writes format, dword offset, and stride into each vfetch whose semantic
// matches an element. dwords are host-endian microcode. Fails when an
// instruction or an element is missing.
bool PatchVertexDeclaration(uint32_t* dwords, uint32_t dword_count,
                            const VertexFetchSemantic* semantics, uint32_t semantic_count,
                            const VertexElement* elements, uint32_t element_count,
                            std::string& reason);

}  // namespace broken_bond::gfx
