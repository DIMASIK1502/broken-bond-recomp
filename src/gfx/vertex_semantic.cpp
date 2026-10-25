#include "gfx/vertex_semantic.h"

#include <unordered_set>
#include <vector>

namespace broken_bond::gfx {
namespace {

uint32_t ReadBe(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         uint32_t(bytes[3]);
}

bool IsExec(uint32_t opcode) {
  return opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4 || opcode == 5 || opcode == 6 ||
         opcode == 13 || opcode == 14;
}

bool CollectVfetches(const uint8_t* ucode, uint32_t ucode_bytes, std::unordered_set<uint32_t>& out,
                     std::string& reason) {
  if (ucode_bytes < 12 || ucode_bytes % 12 != 0) {
    reason = "vertex microcode is not a multiple of an instruction";
    return false;
  }
  const uint32_t slots = ucode_bytes / 12;
  std::vector<uint32_t> dwords(slots * 3);
  for (uint32_t i = 0; i < slots * 3; ++i) {
    dwords[i] = ReadBe(ucode + i * 4);
  }
  uint32_t bound = slots;
  auto pair = [&](uint32_t slot, uint32_t out_words[4]) {
    const uint32_t a = dwords[slot * 3];
    const uint32_t b = dwords[slot * 3 + 1];
    const uint32_t c = dwords[slot * 3 + 2];
    out_words[0] = a;
    out_words[1] = b & 0xFFFFu;
    out_words[2] = (b >> 16) | (c << 16);
    out_words[3] = c >> 16;
  };
  for (uint32_t slot = 0; slot < bound; ++slot) {
    uint32_t words[4];
    pair(slot, words);
    for (int half = 0; half < 2; ++half) {
      const uint32_t word0 = words[half * 2];
      const uint32_t word1 = words[half * 2 + 1];
      if (!IsExec((word1 >> 12) & 0xF)) {
        continue;
      }
      const uint32_t address = word0 & 0xFFF;
      if (address != 0 && address < bound) {
        bound = address;
      }
    }
  }
  for (uint32_t slot = 0; slot < bound; ++slot) {
    uint32_t words[4];
    pair(slot, words);
    for (int half = 0; half < 2; ++half) {
      const uint32_t word0 = words[half * 2];
      const uint32_t word1 = words[half * 2 + 1];
      if (!IsExec((word1 >> 12) & 0xF)) {
        continue;
      }
      uint32_t sequence = (word0 >> 16) & 0xFFF;
      const uint32_t address = word0 & 0xFFF;
      const uint32_t count = (word0 >> 12) & 7;
      for (uint32_t n = 0; n < count; ++n, sequence >>= 2) {
        if ((sequence & 1u) == 0) {
          continue;
        }
        const uint32_t at = (address + n) * 3;
        if (at >= dwords.size()) {
          reason = "vfetch address is outside the microcode";
          return false;
        }
        if ((dwords[at] & 0x1Fu) == 0) {
          out.insert(address + n);
        }
      }
    }
  }
  return true;
}

}  // namespace

const char* VertexUsageName(uint32_t usage) {
  switch (usage) {
    case 0:
      return "position";
    case 1:
      return "blendweight";
    case 2:
      return "blendindices";
    case 3:
      return "normal";
    case 4:
      return "psize";
    case 5:
      return "texcoord";
    case 6:
      return "tangent";
    case 7:
      return "binormal";
    case 10:
      return "color";
    default:
      return "unknown";
  }
}

bool ReadVertexFetchSemantics(const uint8_t* object, size_t size,
                              std::vector<VertexFetchSemantic>& out, std::string& reason) {
  out.clear();
  if (object == nullptr || size < 52 || ReadBe(object + 48) != 0xFFFE0300u) {
    reason = "vertex semantic table is not a vs_3_0 object";
    return false;
  }
  const uint32_t section = ReadBe(object + 4);
  const uint32_t desc = ReadBe(object + 24);
  if (desc + 8 > size || section > size) {
    reason = "vertex semantic descriptor is outside the object";
    return false;
  }
  const uint32_t ucode_offset = ReadBe(object + desc);
  const uint32_t ucode_bytes = ReadBe(object + desc + 4);
  const uint64_t ucode_at = uint64_t(section) + ucode_offset;
  if (ucode_at + ucode_bytes > size) {
    reason = "vertex microcode is outside the object";
    return false;
  }
  std::unordered_set<uint32_t> fetches;
  if (!CollectVfetches(object + ucode_at, ucode_bytes, fetches, reason)) {
    return false;
  }
  if (desc + 8 > section) {
    reason = "vertex semantic blob is empty";
    return false;
  }
  const uint8_t* blob = object + desc + 8;
  const uint32_t blob_bytes = section - (desc + 8);
  if (blob_bytes % 4 != 0) {
    reason = "vertex semantic blob is not dword aligned";
    return false;
  }
  int marker = -1;
  const uint32_t blob_dwords = blob_bytes / 4;
  for (uint32_t i = 0; i < blob_dwords; ++i) {
    if (ReadBe(blob + i * 4) == 0x290u) {
      marker = static_cast<int>(i);
      break;
    }
  }
  if (marker < 0) {
    reason = "vertex semantic marker is missing";
    return false;
  }
  for (uint32_t i = static_cast<uint32_t>(marker) + 1; i < blob_dwords; ++i) {
    const uint32_t word = ReadBe(blob + i * 4);
    const uint32_t instruction = word & 0xFFu;
    if (fetches.find(instruction) == fetches.end()) {
      break;
    }
    VertexFetchSemantic semantic;
    semantic.instruction = instruction;
    semantic.usage = (word >> 12) & 0xFu;
    semantic.usage_index = (word >> 16) & 0xFu;
    semantic.variant = (word >> 20) & 0xFu;
    out.push_back(semantic);
    fetches.erase(instruction);
  }
  if (!fetches.empty() || out.empty()) {
    reason = "vertex semantic table does not match the vfetch instructions";
    return false;
  }
  return true;
}

bool PatchVertexDeclaration(uint32_t* dwords, uint32_t dword_count,
                            const VertexFetchSemantic* semantics, uint32_t semantic_count,
                            const VertexElement* elements, uint32_t element_count,
                            std::string& reason) {
  if (dwords == nullptr || dword_count < 3 || dword_count % 3 != 0 || semantics == nullptr ||
      elements == nullptr || semantic_count == 0 || element_count == 0) {
    reason = "vertex declaration patch has no microcode";
    return false;
  }
  std::vector<const VertexElement*> by_instruction(dword_count / 3, nullptr);
  for (uint32_t i = 0; i < semantic_count; ++i) {
    const VertexFetchSemantic& semantic = semantics[i];
    const VertexElement* element = nullptr;
    for (uint32_t e = 0; e < element_count; ++e) {
      if (elements[e].usage == semantic.usage && elements[e].usage_index == semantic.usage_index) {
        element = &elements[e];
        break;
      }
    }
    if (element == nullptr || semantic.instruction >= by_instruction.size()) {
      reason = "vertex declaration has no element for a vfetch";
      return false;
    }
    by_instruction[semantic.instruction] = element;
  }
  const uint32_t slots = dword_count / 3;
  uint32_t bound = slots;
  auto pair = [&](uint32_t slot, uint32_t out_words[4]) {
    const uint32_t a = dwords[slot * 3];
    const uint32_t b = dwords[slot * 3 + 1];
    const uint32_t c = dwords[slot * 3 + 2];
    out_words[0] = a;
    out_words[1] = b & 0xFFFFu;
    out_words[2] = (b >> 16) | (c << 16);
    out_words[3] = c >> 16;
  };
  auto is_exec = [](uint32_t opcode) {
    return opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4 || opcode == 5 || opcode == 6 ||
           opcode == 13 || opcode == 14;
  };
  for (uint32_t slot = 0; slot < bound; ++slot) {
    uint32_t words[4];
    pair(slot, words);
    for (int half = 0; half < 2; ++half) {
      if (!is_exec((words[half * 2 + 1] >> 12) & 0xF)) {
        continue;
      }
      const uint32_t address = words[half * 2] & 0xFFF;
      if (address != 0 && address < bound) {
        bound = address;
      }
    }
  }
  uint32_t patched = 0;
  for (uint32_t slot = 0; slot < bound; ++slot) {
    uint32_t words[4];
    pair(slot, words);
    for (int half = 0; half < 2; ++half) {
      const uint32_t word0 = words[half * 2];
      const uint32_t word1 = words[half * 2 + 1];
      if (!is_exec((word1 >> 12) & 0xF)) {
        continue;
      }
      uint32_t sequence = (word0 >> 16) & 0xFFF;
      const uint32_t address = word0 & 0xFFF;
      const uint32_t count = (word0 >> 12) & 7;
      for (uint32_t n = 0; n < count; ++n, sequence >>= 2) {
        if ((sequence & 1u) == 0) {
          continue;
        }
        const uint32_t instruction = address + n;
        const uint32_t at = instruction * 3;
        if (at + 2 >= dword_count || (dwords[at] & 0x1Fu) != 0) {
          continue;
        }
        if (instruction >= by_instruction.size() || by_instruction[instruction] == nullptr) {
          reason = "vfetch has no declaration element";
          return false;
        }
        const VertexElement& element = *by_instruction[instruction];
        uint32_t word1 = dwords[at + 1];
        word1 = (word1 & ~(0x3Fu << 16)) | ((element.format & 0x3Fu) << 16);
        if (element.signed_components) {
          word1 |= 1u << 12;
        } else {
          word1 &= ~(1u << 12);
        }
        if (element.integer_components) {
          word1 |= 1u << 13;
        } else {
          word1 &= ~(1u << 13);
        }
        dwords[at + 1] = word1;
        const uint32_t offset = static_cast<uint32_t>(element.offset_words) & 0x7FFFFFu;
        dwords[at + 2] = (dwords[at + 2] & ~0x7FFFFF00u) | (offset << 8);
        dwords[at + 2] = (dwords[at + 2] & ~0xFFu) | (element.stride_words & 0xFFu);
        ++patched;
      }
    }
  }
  if (patched != semantic_count) {
    reason = "vertex declaration did not patch every vfetch";
    return false;
  }
  return true;
}

}  // namespace broken_bond::gfx
