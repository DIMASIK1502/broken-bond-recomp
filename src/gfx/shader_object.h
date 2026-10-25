// Microcode bytes of one vertex shader object, at the guest address
// sub_821B6CE0 copies into the ring.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace broken_bond::gfx {

struct ShaderMicrocode {
  uint32_t guest_address = 0;
  uint32_t byte_count = 0;
};

// guest is physical memory: a guest address is indexed with its low 29 bits.
// shader is the vertex shader object. Dwords in guest are big-endian.
bool ReadVertexShaderMicrocode(const uint8_t* guest, size_t guest_bytes, uint32_t shader,
                               ShaderMicrocode& out, std::vector<uint8_t>& bytes,
                               std::string& reason);

}  // namespace broken_bond::gfx
