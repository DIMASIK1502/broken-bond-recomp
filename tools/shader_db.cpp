// Reads this dump's xeshader.sdb and prints what is in it.
// Does not translate microcode and does not write a shader cache.
//
// Big-endian container, magic 3BDS, version 4.1 on this dump:
//   u32 magic, version, source_count, shader_count, source_offset, shader_offset
//   source_count entries at source_offset:
//     char name[64], u32 checksum, compressed_size, raw_size, index
//     zlib payload of compressed_size bytes
//   32-byte preamble, then shader_count records of 11 big-endian words:
//     payload offset, compressed size, raw size, tag, aux, id, key, 4 unused
//   each payload is zlib. The inflated object is big-endian:
//     u32 magic              0x102A1101 vertex, 0x102A1100 pixel
//     u32 code_section       byte offset of the second section
//     u32 code_section_bytes the two sections add up to the object
//     u32 zero, info_offset, another offset
//     u32 ucode_desc         byte offset of {u32 offset, u32 bytes}
//   ucode offset is relative to code_section. Dword at object offset 48 is
//   the D3D9 version: 0xFFFE0300 vs_3_0, 0xFFFF0300 ps_3_0. The microcode is
//   Xenos control flow packed as two instructions per three dwords.
//   At offset 0x28 there is a big-endian D3DX constant table: float4 and sampler
//   records, 20 bytes each, names relative to that table.
//   When SHADER_DB_TRANSLATE is set, each microcode is translated to SPIR-V in
//   memory. Nothing is written.

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(SHADER_DB_TRANSLATE)
#include <algorithm>
#include <map>
#include <utility>

#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/string/buffer.h>

#if defined(SHADER_DB_VULKAN_SDK)
#include <spirv-tools/libspirv.h>
#endif
#if defined(BROKEN_BOND_HOST_GFX)
#include "gfx/device.h"
#include "gfx/device_shadow.h"
#include "gfx/shader_object.h"
#include "gfx/texture.h"
#include "gfx/vertex_semantic.h"
#endif

REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Unused here. AnalyzeUcode reads it.");

// The depth-stencil path in the translator names these tables. They live in
// the GPU plugin's draw helper, which this tool does not link.
namespace rex::graphics::draw_util {
extern const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
extern const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
}  // namespace rex::graphics::draw_util
#endif

namespace {

[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error(message);
}

uint32_t ReadBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : cur_(data), end_(data + size) {}

  uint32_t Pull(int count) {
    while (bits_ < count) {
      if (cur_ >= end_) {
        Fail("deflate stream ended early");
      }
      acc_ |= uint32_t(*cur_++) << bits_;
      bits_ += 8;
    }
    const uint32_t value = acc_ & ((1u << count) - 1u);
    acc_ >>= count;
    bits_ -= count;
    return value;
  }

  void Align() {
    const int drop = bits_ & 7;
    acc_ >>= drop;
    bits_ -= drop;
  }

  const uint8_t* cursor() const { return cur_; }
  const uint8_t* end() const { return end_; }

 private:
  const uint8_t* cur_;
  const uint8_t* end_;
  uint32_t acc_ = 0;
  int bits_ = 0;
};

struct Huff {
  struct Node {
    int left = -1;
    int right = -1;
    int symbol = -1;
  };
  std::vector<Node> nodes;
  int root = -1;

  int AddNode() {
    nodes.push_back({});
    return static_cast<int>(nodes.size()) - 1;
  }

  void Build(const uint8_t* lengths, int count) {
    nodes.clear();
    root = AddNode();
    int bl_count[16] = {};
    for (int i = 0; i < count; ++i) {
      if (lengths[i] > 15) {
        Fail("huffman length above 15");
      }
      ++bl_count[lengths[i]];
    }
    bl_count[0] = 0;
    int next_code[16] = {};
    int code = 0;
    for (int bits = 1; bits <= 15; ++bits) {
      code = (code + bl_count[bits - 1]) << 1;
      next_code[bits] = code;
    }
    for (int symbol = 0; symbol < count; ++symbol) {
      const int length = lengths[symbol];
      if (length == 0) {
        continue;
      }
      const int assigned = next_code[length]++;
      int node = root;
      // The stream delivers the high bit of the code first.
      for (int bit_index = length - 1; bit_index >= 0; --bit_index) {
        const int bit = (assigned >> bit_index) & 1;
        const bool last = bit_index == 0;
        int child = bit ? nodes[node].right : nodes[node].left;
        if (last) {
          if (child >= 0) {
            Fail("huffman prefix collision");
          }
          child = AddNode();
          (bit ? nodes[node].right : nodes[node].left) = child;
          nodes[child].symbol = symbol;
          break;
        }
        if (child < 0) {
          child = AddNode();
          (bit ? nodes[node].right : nodes[node].left) = child;
        } else if (nodes[child].symbol >= 0) {
          Fail("huffman walked into a leaf");
        }
        node = child;
      }
    }
  }

  int Decode(BitReader& bits) const {
    int node = root;
    for (;;) {
      const int bit = static_cast<int>(bits.Pull(1));
      node = bit ? nodes[node].right : nodes[node].left;
      if (node < 0) {
        Fail("huffman missing child");
      }
      if (nodes[node].symbol >= 0) {
        return nodes[node].symbol;
      }
    }
  }
};

constexpr uint16_t kLengthBase[29] = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistanceBase[30] = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr int kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

void InflateBlock(BitReader& bits, const Huff& literal, const Huff& distance, std::vector<uint8_t>& out,
                   size_t limit) {
  for (;;) {
    const int symbol = literal.Decode(bits);
    if (symbol < 256) {
      if (out.size() >= limit) {
        Fail("inflated shader is longer than its directory size");
      }
      out.push_back(static_cast<uint8_t>(symbol));
      continue;
    }
    if (symbol == 256) {
      return;
    }
    if (symbol > 285) {
      Fail("invalid length symbol");
    }
    const int length_index = symbol - 257;
    const uint32_t length = kLengthBase[length_index] + bits.Pull(kLengthExtra[length_index]);
    const int distance_symbol = distance.Decode(bits);
    if (distance_symbol >= 30) {
      Fail("invalid distance symbol");
    }
    const uint32_t distance =
        kDistanceBase[distance_symbol] + bits.Pull(kDistanceExtra[distance_symbol]);
    if (distance == 0 || distance > out.size()) {
      Fail("distance points outside the inflated shader");
    }
    if (out.size() + length > limit) {
      Fail("inflated shader is longer than its directory size");
    }
    for (uint32_t i = 0; i < length; ++i) {
      out.push_back(out[out.size() - distance]);
    }
  }
}

std::vector<uint8_t> InflateZlib(const uint8_t* data, size_t size, size_t raw_size) {
  if (size < 6) {
    Fail("zlib payload is shorter than a header");
  }
  const uint32_t header = (uint32_t(data[0]) << 8) | data[1];
  if ((header % 31) != 0 || (data[0] & 0x0F) != 8 || (data[1] & 0x20) != 0) {
    Fail("zlib header is not a raw deflate stream");
  }
  BitReader bits(data + 2, size - 2);
  std::vector<uint8_t> out;
  out.reserve(raw_size);
  bool final_block = false;
  while (!final_block) {
    final_block = bits.Pull(1) != 0;
    const uint32_t type = bits.Pull(2);
    if (type == 0) {
      bits.Align();
      const uint32_t length = bits.Pull(16);
      const uint32_t complement = bits.Pull(16);
      if (length != (complement ^ 0xFFFFu)) {
        Fail("stored block length check failed");
      }
      if (out.size() + length > raw_size) {
        Fail("stored block exceeds the directory size");
      }
      for (uint32_t i = 0; i < length; ++i) {
        out.push_back(static_cast<uint8_t>(bits.Pull(8)));
      }
      continue;
    }
    Huff literal;
    Huff distance;
    if (type == 1) {
      uint8_t lit_len[288];
      for (int i = 0; i <= 143; ++i) lit_len[i] = 8;
      for (int i = 144; i <= 255; ++i) lit_len[i] = 9;
      for (int i = 256; i <= 279; ++i) lit_len[i] = 7;
      for (int i = 280; i <= 287; ++i) lit_len[i] = 8;
      uint8_t dist_len[32];
      for (int i = 0; i < 32; ++i) dist_len[i] = 5;
      literal.Build(lit_len, 288);
      distance.Build(dist_len, 32);
    } else if (type == 2) {
      const int hlit = static_cast<int>(bits.Pull(5)) + 257;
      const int hdist = static_cast<int>(bits.Pull(5)) + 1;
      const int hclen = static_cast<int>(bits.Pull(4)) + 4;
      uint8_t code_lengths[19] = {};
      for (int i = 0; i < hclen; ++i) {
        code_lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(bits.Pull(3));
      }
      Huff code_length;
      code_length.Build(code_lengths, 19);
      std::vector<uint8_t> lengths(static_cast<size_t>(hlit + hdist));
      int filled = 0;
      while (filled < hlit + hdist) {
        const int symbol = code_length.Decode(bits);
        if (symbol < 16) {
          lengths[filled++] = static_cast<uint8_t>(symbol);
          continue;
        }
        int repeat = 0;
        uint8_t value = 0;
        if (symbol == 16) {
          if (filled == 0) {
            Fail("repeat code with no previous length");
          }
          value = lengths[filled - 1];
          repeat = static_cast<int>(bits.Pull(2)) + 3;
        } else if (symbol == 17) {
          repeat = static_cast<int>(bits.Pull(3)) + 3;
        } else if (symbol == 18) {
          repeat = static_cast<int>(bits.Pull(7)) + 11;
        } else {
          Fail("invalid code-length symbol");
        }
        if (filled + repeat > hlit + hdist) {
          Fail("code-length repeat runs past the alphabet");
        }
        for (int i = 0; i < repeat; ++i) {
          lengths[filled++] = value;
        }
      }
      literal.Build(lengths.data(), hlit);
      distance.Build(lengths.data() + hlit, hdist);
    } else {
      Fail("invalid deflate block type");
    }
    InflateBlock(bits, literal, distance, out, raw_size);
  }
  if (out.size() != raw_size) {
    Fail("inflated size does not match the directory");
  }
  if (bits.cursor() + 4 > bits.end()) {
    Fail("zlib checksum is missing");
  }
  uint32_t s1 = 1;
  uint32_t s2 = 0;
  for (uint8_t byte : out) {
    s1 = (s1 + byte) % 65521u;
    s2 = (s2 + s1) % 65521u;
  }
  const uint32_t adler = (s2 << 16) | s1;
  const uint8_t* sum = bits.cursor();
  const uint32_t stored = (uint32_t(sum[0]) << 24) | (uint32_t(sum[1]) << 16) |
                          (uint32_t(sum[2]) << 8) | sum[3];
  if (adler != stored) {
    Fail("zlib checksum does not match");
  }
  return out;
}

struct Source {
  std::string name;
  uint32_t raw_size = 0;
};

struct ShaderRecord {
  uint32_t offset = 0;
  uint32_t compressed_size = 0;
  uint32_t raw_size = 0;
  uint32_t tag = 0;
};

struct UcodeSpan {
  const uint32_t* dwords = nullptr;
  uint32_t dword_count = 0;
};

// Every control-flow slot up to the first exec address must point inside this
// microcode. That address is the control-flow length the SDK uses. The dwords
// are still big-endian in the file.
UcodeSpan LocateUcode(const uint8_t* object, size_t size, uint32_t token) {
  if (size < 28 || size % 4 != 0) {
    Fail("shader object is truncated");
  }
  const uint32_t magic = ReadBe32(object);
  if (token == 0xFFFE0300u && magic != 0x102A1101u) {
    Fail("vertex shader magic does not match vs_3_0");
  }
  if (token == 0xFFFF0300u && magic != 0x102A1100u) {
    Fail("pixel shader magic does not match ps_3_0");
  }
  const uint32_t section = ReadBe32(object + 4);
  const uint32_t section_bytes = ReadBe32(object + 8);
  const uint32_t desc = ReadBe32(object + 24);
  if (section + section_bytes != size || desc + 8 > size) {
    Fail("shader sections do not cover the object");
  }
  const uint32_t ucode_offset = ReadBe32(object + desc);
  const uint32_t ucode_bytes = ReadBe32(object + desc + 4);
  const uint32_t ucode_at = section + ucode_offset;
  if (ucode_bytes < 12 || ucode_bytes % 12 != 0 || ucode_at + ucode_bytes > size) {
    Fail("shader microcode is outside the object");
  }
  if (ucode_at % 4 != 0) {
    Fail("shader microcode is not dword aligned");
  }
  const uint32_t slots = ucode_bytes / 12;
  auto ExecAddress = [slots](uint32_t word0, uint32_t word1) -> uint32_t {
    const uint32_t opcode = (word1 >> 12) & 0xF;
    const bool exec = opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4 || opcode == 5 ||
                      opcode == 6 || opcode == 13 || opcode == 14;
    if (!exec) {
      return 0;
    }
    const uint32_t address = word0 & 0xFFF;
    if (address == 0 || address > slots) {
      Fail("shader control flow points outside the microcode");
    }
    return address;
  };
  // Same bound the SDK uses: the first exec address is the control-flow length.
  uint32_t bound = slots;
  bool saw_exec = false;
  for (uint32_t slot = 0; slot < bound; ++slot) {
    const uint32_t at = ucode_at + slot * 12;
    const uint32_t dword_0 = ReadBe32(object + at);
    const uint32_t dword_1 = ReadBe32(object + at + 4);
    const uint32_t dword_2 = ReadBe32(object + at + 8);
    const uint32_t first = ExecAddress(dword_0, dword_1 & 0xFFFF);
    const uint32_t second = ExecAddress((dword_1 >> 16) | (dword_2 << 16), dword_2 >> 16);
    if (first != 0) {
      if (first < bound) {
        bound = first;
      }
      saw_exec = true;
    }
    if (second != 0) {
      if (second < bound) {
        bound = second;
      }
      saw_exec = true;
    }
  }
  if (!saw_exec || bound == 0) {
    Fail("shader microcode does not start with control flow");
  }
  return {reinterpret_cast<const uint32_t*>(object + ucode_at), ucode_bytes / 4};
}

struct ConstantStats {
  uint32_t float4 = 0;
  uint32_t sampler = 0;
  std::set<std::string> names;
};

// Big-endian D3DXSHADER_CONSTANTTABLE at object offset 0x28.
void ReadConstants(const uint8_t* object, size_t size, uint32_t token, ConstantStats& stats) {
  constexpr uint32_t kTable = 0x28;
  if (size < kTable + 28) {
    Fail("shader constant table is missing");
  }
  const uint32_t table_size = ReadBe32(object + kTable);
  const uint32_t version = ReadBe32(object + kTable + 8);
  const uint32_t count = ReadBe32(object + kTable + 12);
  const uint32_t info = ReadBe32(object + kTable + 16);
  const uint32_t flags = ReadBe32(object + kTable + 20);
  const uint32_t target = ReadBe32(object + kTable + 24);
  if (table_size != 0x1C || version != token || flags != 0 || (count != 0 && info != 0x1C)) {
    Fail("shader constant table header does not match");
  }
  if (kTable + target + 6 > size) {
    Fail("shader target name is outside the object");
  }
  const char* expected = token == 0xFFFE0300u ? "vs_3_0" : "ps_3_0";
  for (int i = 0; i < 6; ++i) {
    if (object[kTable + target + i] != static_cast<uint8_t>(expected[i])) {
      Fail("shader target name does not match the version");
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = kTable + info + i * 20;
    if (at + 20 > size) {
      Fail("shader constant record is outside the object");
    }
    const uint32_t name_off = ReadBe32(object + at);
    const uint32_t reg = ReadBe32(object + at + 4);
    const uint32_t reg_count = ReadBe32(object + at + 8);
    const uint32_t set = reg >> 16;
    if ((set != 2 && set != 3) || (reg_count & 0xFFFF) != 0 || kTable + name_off >= size) {
      Fail("shader constant record is not a float4 or sampler");
    }
    const uint8_t* name_bytes = object + kTable + name_off;
    size_t length = 0;
    while (kTable + name_off + length < size && name_bytes[length] != 0) {
      if (name_bytes[length] < 32 || name_bytes[length] > 126) {
        Fail("shader constant name is not ascii");
      }
      ++length;
    }
    if (length == 0) {
      Fail("shader constant name is empty");
    }
    stats.names.emplace(reinterpret_cast<const char*>(name_bytes), length);
    if (set == 2) {
      ++stats.float4;
    } else {
      ++stats.sampler;
    }
  }
}

#if defined(SHADER_DB_TRANSLATE)
struct TranslateStats {
  uint32_t ok = 0;
  uint32_t failed = 0;
  uint32_t dynamic_registers = 0;
  uint32_t memexport = 0;
  uint32_t depth = 0;
  uint32_t texture_shaders = 0;
  uint32_t texture_max = 0;
  uint32_t sampler_shaders = 0;
  uint32_t sampler_max = 0;
  uint32_t vertex_texture_shaders = 0;
  uint32_t vertex_texture_max = 0;
  uint32_t vertex_sampler_max = 0;
  uint32_t pixel_texture_max = 0;
  uint32_t pixel_sampler_max = 0;
  uint32_t float_shaders = 0;
  uint32_t float_max = 0;
  uint64_t spirv_bytes = 0;
  std::map<uint32_t, uint32_t> versions;
  std::map<uint32_t, uint32_t> capabilities;
  std::map<std::string, uint32_t> extensions;
  std::map<uint64_t, uint32_t> descriptors;
  std::map<std::string, uint32_t> fatals;
  struct GuestDraw {
    std::vector<uint32_t> words;
    uint32_t images = 0;
    uint32_t samplers = 0;
    bool interpolate = false;
    bool vertex = false;
    bool declare_test = false;
    uint32_t interpolators = 0;
    std::vector<broken_bond::gfx::VertexStream> streams;
  };
  std::vector<GuestDraw> guest_alu;
  std::vector<GuestDraw> guest_textured;
  std::vector<GuestDraw> guest_interpolated;
  std::vector<GuestDraw> guest_vertices;
  std::vector<GuestDraw> guest_declared;
  std::vector<GuestDraw> guest_packed;
  std::vector<GuestDraw> guest_short;
  uint32_t guest_textured_seen = 0;
  uint32_t guest_interpolated_seen = 0;
  uint32_t guest_vertex_seen = 0;
  uint32_t guest_fetch_seen = 0;
  uint32_t vb_shaders = 0;
  uint32_t vb_with_binding = 0;
  uint32_t vb_attr_stride = 0;
  uint32_t vb_float = 0;
  uint32_t vb_index0 = 0;
  uint32_t format_hits[64] = {};
  uint32_t example_format = 0xFFFFFFFFu;
  uint32_t example_stride = 0;
  uint32_t example_offset = 0;
  uint32_t example_index = 0xFFFFFFFFu;
  uint32_t example_swizzle = 0xFFFFFFFFu;
  uint32_t example_fetch = 0;
  uint32_t disasm_vfetch = 0;
  uint32_t disasm_format = 0;
  uint32_t disasm_stride = 0;
  std::string disasm_sample;
  uint32_t sample_dwords[48] = {};
  uint32_t sample_dword_count = 0;
};

struct HostProbe {
  HMODULE tools = nullptr;
  HMODULE vulkan = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkDestroyInstance destroy_instance = nullptr;
  PFN_vkDestroyDevice destroy_device = nullptr;
  PFN_vkCreateShaderModule create_module = nullptr;
  PFN_vkDestroyShaderModule destroy_module = nullptr;
  PFN_vkCreateGraphicsPipelines create_pipelines = nullptr;
  PFN_vkDestroyPipeline destroy_pipeline = nullptr;
  PFN_vkCreatePipelineLayout create_pipeline_layout = nullptr;
  PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
  PFN_vkCreateDescriptorSetLayout create_set_layout = nullptr;
  PFN_vkDestroyDescriptorSetLayout destroy_set_layout = nullptr;
  VkDescriptorSetLayout set_layouts[3] = {};
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule empty_fragment = VK_NULL_HANDLE;
  VkDescriptorSetLayout pixel_sets[3] = {};
  VkShaderModule position_vertex = VK_NULL_HANDLE;
  std::map<uint32_t, VkPipelineLayout> pixel_layouts;
  std::vector<VkDescriptorSetLayout> pixel_texture_sets;
#if defined(SHADER_DB_VULKAN_SDK)
  spv_context context = nullptr;
  using ContextCreate = spv_context (*)(spv_target_env);
  using ContextDestroy = void (*)(spv_context);
  using OptionsCreate = spv_validator_options (*)();
  using OptionsDestroy = void (*)(spv_validator_options);
  using SetScalarBlockLayout = void (*)(spv_validator_options, bool);
  using ValidateWithOptions = spv_result_t (*)(const spv_const_context, const spv_const_validator_options,
                                              const spv_const_binary, spv_diagnostic*);
  using DiagnosticDestroy = void (*)(spv_diagnostic);
  ContextDestroy destroy_context = nullptr;
  OptionsDestroy destroy_options = nullptr;
  ValidateWithOptions validate = nullptr;
  DiagnosticDestroy destroy_diagnostic = nullptr;
  spv_validator_options options = nullptr;
#endif
  std::string device_name;
  std::string driver_name;
  std::string vulkan_reason;
  std::string validator_reason;
  uint32_t api = 0;
  uint32_t storage_range = 0;
  bool preserve = false;
  bool flush = false;
  bool rte = false;
  bool sample_rate = false;
  bool demote = false;
  bool scalar_block = false;
  bool standard_layout = false;
  bool dynamic_rendering = false;
  uint32_t pipeline_ok = 0;
  uint32_t pipeline_failed = 0;
  std::map<int32_t, uint32_t> pipeline_results;
  uint32_t pixel_ok = 0;
  uint32_t pixel_failed = 0;
  std::map<int32_t, uint32_t> pixel_results;

  struct ShaderStage {
    bool vertex = false;
    uint32_t images = 0;
    uint32_t samplers = 0;
    uint32_t color_mask = 0;
    bool depth = false;
  };
  uint32_t val_ok = 0;
  uint32_t val_failed = 0;
  uint32_t module_ok = 0;
  uint32_t module_failed = 0;
  std::map<std::string, uint32_t> val_messages;
  std::map<int32_t, uint32_t> module_results;

  HostProbe() = default;
  HostProbe(const HostProbe&) = delete;
  HostProbe& operator=(const HostProbe&) = delete;

  ~HostProbe() {
    if (device != VK_NULL_HANDLE) {
      if (destroy_module != nullptr && empty_fragment != VK_NULL_HANDLE) {
        destroy_module(device, empty_fragment, nullptr);
      }
      if (destroy_module != nullptr && position_vertex != VK_NULL_HANDLE) {
        destroy_module(device, position_vertex, nullptr);
      }
      if (destroy_pipeline_layout != nullptr && pipeline_layout != VK_NULL_HANDLE) {
        destroy_pipeline_layout(device, pipeline_layout, nullptr);
      }
      if (destroy_pipeline_layout != nullptr) {
        for (const auto& layout : pixel_layouts) {
          destroy_pipeline_layout(device, layout.second, nullptr);
        }
      }
      if (destroy_set_layout != nullptr) {
        for (VkDescriptorSetLayout layout : set_layouts) {
          if (layout != VK_NULL_HANDLE) {
            destroy_set_layout(device, layout, nullptr);
          }
        }
        for (VkDescriptorSetLayout layout : pixel_sets) {
          if (layout != VK_NULL_HANDLE) {
            destroy_set_layout(device, layout, nullptr);
          }
        }
        for (VkDescriptorSetLayout layout : pixel_texture_sets) {
          destroy_set_layout(device, layout, nullptr);
        }
      }
    }
    if (destroy_device != nullptr && device != VK_NULL_HANDLE) {
      destroy_device(device, nullptr);
    }
    if (destroy_instance != nullptr && instance != VK_NULL_HANDLE) {
      destroy_instance(instance, nullptr);
    }
#if defined(SHADER_DB_VULKAN_SDK)
    if (destroy_options != nullptr && options != nullptr) {
      destroy_options(options);
    }
    if (destroy_context != nullptr && context != nullptr) {
      destroy_context(context);
    }
    if (tools != nullptr) {
      FreeLibrary(tools);
    }
#endif
    if (vulkan != nullptr) {
      FreeLibrary(vulkan);
    }
  }

  void Open();
  void PrepareVertexPipelines();
  void PreparePixelPipelines();
  VkPipelineLayout PixelLayout(uint32_t images, uint32_t samplers);
  void LinkGraphics(VkShaderModule vertex_module, VkShaderModule fragment_module,
                    VkPipelineLayout layout, uint32_t color_count, bool depth, uint32_t& ok,
                    uint32_t& failed, std::map<int32_t, uint32_t>& results);
  void Check(const std::vector<uint8_t>& binary, const ShaderStage& stage);
};

void HostProbe::Open() {
#if defined(SHADER_DB_VULKAN_SDK)
  const std::string tools_path = std::string(SHADER_DB_VULKAN_SDK) + "\\Bin\\SPIRV-Tools-shared.dll";
  tools = LoadLibraryExA(tools_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (tools == nullptr) {
    validator_reason = "cannot load SPIRV-Tools-shared.dll";
  } else {
    auto create_context = reinterpret_cast<ContextCreate>(GetProcAddress(tools, "spvContextCreate"));
    destroy_context = reinterpret_cast<ContextDestroy>(GetProcAddress(tools, "spvContextDestroy"));
    auto create_options = reinterpret_cast<OptionsCreate>(GetProcAddress(tools, "spvValidatorOptionsCreate"));
    destroy_options = reinterpret_cast<OptionsDestroy>(GetProcAddress(tools, "spvValidatorOptionsDestroy"));
    auto set_scalar = reinterpret_cast<SetScalarBlockLayout>(
        GetProcAddress(tools, "spvValidatorOptionsSetScalarBlockLayout"));
    validate = reinterpret_cast<ValidateWithOptions>(GetProcAddress(tools, "spvValidateWithOptions"));
    destroy_diagnostic =
        reinterpret_cast<DiagnosticDestroy>(GetProcAddress(tools, "spvDiagnosticDestroy"));
    if (create_context == nullptr || destroy_context == nullptr || create_options == nullptr ||
        destroy_options == nullptr || set_scalar == nullptr || validate == nullptr ||
        destroy_diagnostic == nullptr) {
      validator_reason = "SPIRV-Tools exports are missing";
    } else {
      context = create_context(SPV_ENV_VULKAN_1_2);
      options = create_options();
      if (context == nullptr || options == nullptr) {
        validator_reason = "spvContextCreate failed";
      } else {
        set_scalar(options, true);
      }
    }
  }
#else
  validator_reason = "Vulkan SDK path is not set";
#endif

  vulkan = LoadLibraryA("vulkan-1.dll");
  if (vulkan == nullptr) {
    vulkan_reason = "vulkan-1.dll is not installed";
    return;
  }
  auto get_instance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
  if (get_instance == nullptr) {
    vulkan_reason = "vkGetInstanceProcAddr is missing";
    return;
  }
  auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      get_instance(nullptr, "vkEnumerateInstanceVersion"));
  uint32_t loader_version = VK_API_VERSION_1_0;
  if (enumerate_version != nullptr) {
    enumerate_version(&loader_version);
  }
  const uint32_t requested = loader_version >= VK_API_VERSION_1_3   ? VK_API_VERSION_1_3
                             : loader_version >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2
                                                                    : VK_API_VERSION_1_1;
  auto create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(get_instance(nullptr, "vkCreateInstance"));
  if (create_instance == nullptr) {
    vulkan_reason = "vkCreateInstance is missing";
    return;
  }
  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.apiVersion = requested;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &application;
  const VkResult created = create_instance(&instance_info, nullptr, &instance);
  if (created != VK_SUCCESS) {
    vulkan_reason = "vkCreateInstance returned " + std::to_string(int(created));
    return;
  }
  destroy_instance =
      reinterpret_cast<PFN_vkDestroyInstance>(get_instance(instance, "vkDestroyInstance"));
  auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      get_instance(instance, "vkEnumeratePhysicalDevices"));
  auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      get_instance(instance, "vkGetPhysicalDeviceProperties2"));
  auto get_features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
      get_instance(instance, "vkGetPhysicalDeviceFeatures2"));
  auto get_queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      get_instance(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  auto create_device =
      reinterpret_cast<PFN_vkCreateDevice>(get_instance(instance, "vkCreateDevice"));
  auto get_device = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_instance(instance, "vkGetDeviceProcAddr"));
  if (destroy_instance == nullptr || enumerate_devices == nullptr || get_properties == nullptr ||
      get_features == nullptr || get_queues == nullptr || create_device == nullptr ||
      get_device == nullptr) {
    vulkan_reason = "instance exports are missing";
    return;
  }

  uint32_t device_count = 0;
  enumerate_devices(instance, &device_count, nullptr);
  std::vector<VkPhysicalDevice> devices(device_count);
  if (device_count == 0 ||
      enumerate_devices(instance, &device_count, devices.data()) != VK_SUCCESS) {
    vulkan_reason = "no physical device";
    return;
  }
  VkPhysicalDevice physical = devices[0];
  int best_rank = -1;
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    get_properties(candidate, &properties);
    const int rank = properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                     : properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                         ? 1
                         : 0;
    if (rank > best_rank) {
      best_rank = rank;
      physical = candidate;
      api = properties.properties.apiVersion;
      device_name = properties.properties.deviceName;
      storage_range = properties.properties.limits.maxStorageBufferRange;
    }
  }

  VkPhysicalDeviceFloatControlsProperties float_controls{};
  float_controls.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
  VkPhysicalDeviceVulkan12Properties properties12{};
  properties12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
  float_controls.pNext = &properties12;
  VkPhysicalDeviceProperties2 properties{};
  properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  if (api >= VK_API_VERSION_1_2) {
    properties.pNext = &float_controls;
  }
  get_properties(physical, &properties);
  api = properties.properties.apiVersion;
  device_name = properties.properties.deviceName;
  storage_range = properties.properties.limits.maxStorageBufferRange;
  if (api >= VK_API_VERSION_1_2) {
    preserve = float_controls.shaderSignedZeroInfNanPreserveFloat32 == VK_TRUE;
    flush = float_controls.shaderDenormFlushToZeroFloat32 == VK_TRUE;
    rte = float_controls.shaderRoundingModeRTEFloat32 == VK_TRUE;
    driver_name = properties12.driverName;
  }

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  VkPhysicalDeviceFeatures2 features{};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  if (api >= VK_API_VERSION_1_2) {
    features.pNext = &features12;
    if (api >= VK_API_VERSION_1_3) {
      features12.pNext = &features13;
    }
  }
  get_features(physical, &features);
  sample_rate = features.features.sampleRateShading == VK_TRUE;
  demote = api >= VK_API_VERSION_1_3 && features13.shaderDemoteToHelperInvocation == VK_TRUE;
  if (api >= VK_API_VERSION_1_2) {
    scalar_block = features12.scalarBlockLayout == VK_TRUE;
    standard_layout = features12.uniformBufferStandardLayout == VK_TRUE;
  }

  uint32_t family_count = 0;
  get_queues(physical, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  get_queues(physical, &family_count, families.data());
  uint32_t graphics_family = UINT32_MAX;
  for (uint32_t i = 0; i < family_count; ++i) {
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      graphics_family = i;
      break;
    }
  }
  if (graphics_family == UINT32_MAX) {
    vulkan_reason = "no graphics queue";
    return;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{};
  queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue.queueFamilyIndex = graphics_family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures enabled{};
  enabled.sampleRateShading = sample_rate ? VK_TRUE : VK_FALSE;
  VkPhysicalDeviceVulkan12Features enabled12{};
  enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  enabled12.scalarBlockLayout = scalar_block ? VK_TRUE : VK_FALSE;
  enabled12.uniformBufferStandardLayout = standard_layout ? VK_TRUE : VK_FALSE;
  VkPhysicalDeviceVulkan13Features enabled13{};
  enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  enabled13.shaderDemoteToHelperInvocation = demote ? VK_TRUE : VK_FALSE;
  dynamic_rendering = api >= VK_API_VERSION_1_3 && features13.dynamicRendering == VK_TRUE;
  enabled13.dynamicRendering = dynamic_rendering ? VK_TRUE : VK_FALSE;
  if (api >= VK_API_VERSION_1_3) {
    enabled12.pNext = &enabled13;
  }
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue;
  device_info.pEnabledFeatures = &enabled;
  if (api >= VK_API_VERSION_1_2) {
    device_info.pNext = &enabled12;
  }
  const VkResult device_result = create_device(physical, &device_info, nullptr, &device);
  if (device_result != VK_SUCCESS) {
    vulkan_reason = "vkCreateDevice returned " + std::to_string(int(device_result));
    return;
  }
  destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(get_device(device, "vkDestroyDevice"));
  create_module =
      reinterpret_cast<PFN_vkCreateShaderModule>(get_device(device, "vkCreateShaderModule"));
  destroy_module =
      reinterpret_cast<PFN_vkDestroyShaderModule>(get_device(device, "vkDestroyShaderModule"));
  create_pipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(get_device(device, "vkCreateGraphicsPipelines"));
  destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(get_device(device, "vkDestroyPipeline"));
  create_pipeline_layout =
      reinterpret_cast<PFN_vkCreatePipelineLayout>(get_device(device, "vkCreatePipelineLayout"));
  destroy_pipeline_layout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(get_device(device, "vkDestroyPipelineLayout"));
  create_set_layout =
      reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(get_device(device, "vkCreateDescriptorSetLayout"));
  destroy_set_layout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
      get_device(device, "vkDestroyDescriptorSetLayout"));
  if (destroy_device == nullptr || create_module == nullptr || destroy_module == nullptr) {
    vulkan_reason = "device exports are missing";
    return;
  }
  if (dynamic_rendering && create_pipelines != nullptr && destroy_pipeline != nullptr &&
      create_pipeline_layout != nullptr && destroy_pipeline_layout != nullptr &&
      create_set_layout != nullptr && destroy_set_layout != nullptr) {
    PrepareVertexPipelines();
    PreparePixelPipelines();
  }
}

void HostProbe::PrepareVertexPipelines() {
  VkDescriptorSetLayoutBinding shared{};
  shared.binding = 0;
  shared.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared.descriptorCount = 1;
  shared.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayoutCreateInfo shared_info{};
  shared_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_info.bindingCount = 1;
  shared_info.pBindings = &shared;

  VkDescriptorSetLayoutBinding constants[4]{};
  const uint32_t constant_bindings[4] = {0, 1, 3, 4};
  for (int i = 0; i < 4; ++i) {
    constants[i].binding = constant_bindings[i];
    constants[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants[i].descriptorCount = 1;
    constants[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  }
  VkDescriptorSetLayoutCreateInfo constant_info{};
  constant_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  constant_info.bindingCount = 4;
  constant_info.pBindings = constants;

  VkDescriptorSetLayoutBinding textures[3]{};
  textures[0].binding = 0;
  textures[1].binding = 1;
  textures[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  textures[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  textures[2].binding = 2;
  textures[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  for (VkDescriptorSetLayoutBinding& binding : textures) {
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  }
  VkDescriptorSetLayoutCreateInfo texture_info{};
  texture_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  texture_info.bindingCount = 3;
  texture_info.pBindings = textures;

  const VkDescriptorSetLayoutCreateInfo infos[3] = {shared_info, constant_info, texture_info};
  for (int i = 0; i < 3; ++i) {
    if (create_set_layout(device, &infos[i], nullptr, &set_layouts[i]) != VK_SUCCESS) {
      vulkan_reason = "descriptor set layout was rejected";
      return;
    }
  }
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 3;
  layout_info.pSetLayouts = set_layouts;
  if (create_pipeline_layout(device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
    vulkan_reason = "pipeline layout was rejected";
    return;
  }

  // Fragment stage with no inputs, so a vertex shader can link without a paired pixel shader.
  static constexpr uint32_t kEmptyFragment[] = {
      0x07230203, 0x00010500, 0x00070000, 0x00000005, 0x00000000, 0x00020011, 0x00000001,
      0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000004, 0x00000001, 0x6e69616d,
      0x00000000, 0x00030010, 0x00000001, 0x00000007, 0x00020013, 0x00000002, 0x00030021,
      0x00000003, 0x00000002, 0x00050036, 0x00000002, 0x00000001, 0x00000000, 0x00000003,
      0x000200f8, 0x00000004, 0x000100fd, 0x00010038};
  VkShaderModuleCreateInfo module_info{};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = sizeof(kEmptyFragment);
  module_info.pCode = kEmptyFragment;
  if (create_module(device, &module_info, nullptr, &empty_fragment) != VK_SUCCESS) {
    vulkan_reason = "empty fragment shader was rejected";
  }
}

void HostProbe::PreparePixelPipelines() {
  VkDescriptorSetLayoutBinding shared{};
  shared.binding = 0;
  shared.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared.descriptorCount = 1;
  shared.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutBinding constants[4]{};
  const uint32_t constant_bindings[4] = {0, 2, 3, 4};
  for (int i = 0; i < 4; ++i) {
    constants[i].binding = constant_bindings[i];
    constants[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants[i].descriptorCount = 1;
    constants[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo shared_info{};
  shared_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_info.bindingCount = 1;
  shared_info.pBindings = &shared;
  VkDescriptorSetLayoutCreateInfo constant_info{};
  constant_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  constant_info.bindingCount = 4;
  constant_info.pBindings = constants;
  VkDescriptorSetLayoutCreateInfo empty_info{};
  empty_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  const VkDescriptorSetLayoutCreateInfo infos[3] = {shared_info, constant_info, empty_info};
  for (int i = 0; i < 3; ++i) {
    if (create_set_layout(device, &infos[i], nullptr, &pixel_sets[i]) != VK_SUCCESS) {
      vulkan_reason = "pixel descriptor set layout was rejected";
      return;
    }
  }

  // Vertex stage that only writes gl_Position. Default pixel modifications declare no
  // interpolator inputs, so this stage matches them.
  static constexpr uint32_t kPositionVertex[] = {
      0x07230203, 0x00010500, 0x00070000, 0x0000000b, 0x00000000, 0x00020011, 0x00000001,
      0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000000, 0x00000001, 0x6e69616d,
      0x00000000, 0x00000002, 0x00040047, 0x00000002, 0x0000000b, 0x00000000, 0x00020013,
      0x00000003, 0x00030021, 0x00000004, 0x00000003, 0x00030016, 0x00000005, 0x00000020,
      0x00040017, 0x00000006, 0x00000005, 0x00000004, 0x00040020, 0x00000007, 0x00000003,
      0x00000006, 0x0004002b, 0x00000005, 0x00000008, 0x00000000, 0x0007002c, 0x00000006,
      0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00000008, 0x0004003b, 0x00000007,
      0x00000002, 0x00000003, 0x00050036, 0x00000003, 0x00000001, 0x00000000, 0x00000004,
      0x000200f8, 0x0000000a, 0x0003003e, 0x00000002, 0x00000009, 0x000100fd, 0x00010038};
  VkShaderModuleCreateInfo module_info{};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = sizeof(kPositionVertex);
  module_info.pCode = kPositionVertex;
  if (create_module(device, &module_info, nullptr, &position_vertex) != VK_SUCCESS) {
    vulkan_reason = "position vertex shader was rejected";
  }
}

VkPipelineLayout HostProbe::PixelLayout(uint32_t images, uint32_t samplers) {
  if (pixel_sets[0] == VK_NULL_HANDLE || images > 32 || samplers > 32) {
    return VK_NULL_HANDLE;
  }
  const uint32_t key = images | (samplers << 8);
  const auto found = pixel_layouts.find(key);
  if (found != pixel_layouts.end()) {
    return found->second;
  }
  std::vector<VkDescriptorSetLayoutBinding> bindings(static_cast<size_t>(images) + samplers);
  for (uint32_t i = 0; i < images; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  for (uint32_t i = 0; i < samplers; ++i) {
    bindings[static_cast<size_t>(images) + i].binding = images + i;
    bindings[static_cast<size_t>(images) + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[static_cast<size_t>(images) + i].descriptorCount = 1;
    bindings[static_cast<size_t>(images) + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo texture_info{};
  texture_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  texture_info.bindingCount = static_cast<uint32_t>(bindings.size());
  texture_info.pBindings = bindings.empty() ? nullptr : bindings.data();
  VkDescriptorSetLayout textures = VK_NULL_HANDLE;
  if (create_set_layout(device, &texture_info, nullptr, &textures) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  pixel_texture_sets.push_back(textures);
  const VkDescriptorSetLayout sets[4] = {pixel_sets[0], pixel_sets[1], pixel_sets[2], textures};
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 4;
  layout_info.pSetLayouts = sets;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (create_pipeline_layout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  pixel_layouts.emplace(key, layout);
  return layout;
}

void HostProbe::LinkGraphics(VkShaderModule vertex_module, VkShaderModule fragment_module,
                             VkPipelineLayout layout, uint32_t color_count, bool depth,
                             uint32_t& ok, uint32_t& failed, std::map<int32_t, uint32_t>& results) {
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_module;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment_module;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {1, 1}};
  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = &viewport;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState attachments[4]{};
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = color_count;
  blend.pAttachments = color_count == 0 ? nullptr : attachments;
  VkPipelineDepthStencilStateCreateInfo depth_state{};
  depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  VkFormat color_formats[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = color_count;
  rendering.pColorAttachmentFormats = color_count == 0 ? nullptr : color_formats;
  rendering.depthAttachmentFormat = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
  VkGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.pNext = &rendering;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = depth ? &depth_state : nullptr;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  const VkResult pipeline_result =
      create_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
  if (pipeline_result == VK_SUCCESS) {
    ++ok;
    destroy_pipeline(device, pipeline, nullptr);
  } else {
    ++failed;
    ++results[int32_t(pipeline_result)];
  }
}

void HostProbe::Check(const std::vector<uint8_t>& binary, const ShaderStage& stage) {
  const auto* words = reinterpret_cast<const uint32_t*>(binary.data());
  const size_t word_count = binary.size() / 4;
#if defined(SHADER_DB_VULKAN_SDK)
  if (context != nullptr && options != nullptr && validate != nullptr) {
    spv_const_binary_t view{words, word_count};
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t result = validate(context, options, &view, &diagnostic);
    if (result == SPV_SUCCESS) {
      ++val_ok;
    } else {
      ++val_failed;
      if (diagnostic != nullptr && diagnostic->error != nullptr) {
        ++val_messages[diagnostic->error];
      } else {
        ++val_messages["validation failed without a message"];
      }
    }
    if (destroy_diagnostic != nullptr && diagnostic != nullptr) {
      destroy_diagnostic(diagnostic);
    }
  }
#else
  (void)words;
  (void)word_count;
#endif
  if (create_module == nullptr) {
    return;
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = binary.size();
  info.pCode = words;
  VkShaderModule module = VK_NULL_HANDLE;
  const VkResult result = create_module(device, &info, nullptr, &module);
  if (result == VK_SUCCESS) {
    ++module_ok;
    if (create_pipelines != nullptr && stage.vertex && pipeline_layout != VK_NULL_HANDLE &&
        empty_fragment != VK_NULL_HANDLE) {
      LinkGraphics(module, empty_fragment, pipeline_layout, 0, false, pipeline_ok, pipeline_failed,
                   pipeline_results);
    } else if (create_pipelines != nullptr && !stage.vertex &&
               position_vertex != VK_NULL_HANDLE) {
      const VkPipelineLayout layout = PixelLayout(stage.images, stage.samplers);
      uint32_t color_count = 0;
      for (uint32_t target = 0; target < 4; ++target) {
        if ((stage.color_mask & (uint32_t(1) << target)) != 0) {
          color_count = target + 1;
        }
      }
      if (layout == VK_NULL_HANDLE || color_count > 4) {
        ++pixel_failed;
      } else {
        LinkGraphics(position_vertex, module, layout, color_count, stage.depth, pixel_ok,
                     pixel_failed, pixel_results);
      }
    }
    destroy_module(device, module, nullptr);
  } else {
    ++module_failed;
    ++module_results[int32_t(result)];
  }
}

const char* CapabilityName(uint32_t capability) {
  switch (capability) {
    case 1:
      return "Shader";
    case 32:
      return "ClipDistance";
    case 33:
      return "CullDistance";
    case 35:
      return "SampleRateShading";
    case 51:
      return "DerivativeControl";
    case 4464:
      return "DenormPreserve";
    case 4465:
      return "DenormFlushToZero";
    case 4466:
      return "SignedZeroInfNanPreserve";
    case 4467:
      return "RoundingModeRTE";
    case 5363:
      return "FragmentShaderSampleInterlockEXT";
    case 5379:
      return "DemoteToHelperInvocationEXT";
    default:
      return nullptr;
  }
}

// Walks one module. A broken instruction stream is a tool failure: the
// translator said the shader was valid.
void NoteModule(const std::vector<uint8_t>& binary, bool vertex,
                const rex::graphics::SpirvShader& shader, TranslateStats& stats) {
  if (binary.size() % 4 != 0 || binary.size() < 20) {
    Fail("spirv module is shorter than its header");
  }
  const auto* words = reinterpret_cast<const uint32_t*>(binary.data());
  const size_t word_count = binary.size() / 4;
  ++stats.versions[words[1]];

  std::map<uint32_t, uint32_t> set_by_id;
  std::map<uint32_t, uint32_t> binding_by_id;
  std::set<uint32_t> seen_capabilities;
  std::set<std::string> seen_extensions;
  std::set<uint64_t> seen_descriptors;
  uint32_t entries = 0;
  size_t cursor = 5;
  while (cursor < word_count) {
    const uint32_t instruction = words[cursor];
    const uint32_t count = instruction >> 16;
    const uint32_t opcode = instruction & 0xFFFFu;
    if (count == 0 || cursor + count > word_count) {
      Fail("spirv instruction runs past the module");
    }
    if (opcode == 17 && count >= 2) {
      seen_capabilities.insert(words[cursor + 1]);
    } else if (opcode == 10 && count >= 2) {
      std::string name;
      for (uint32_t i = 1; i < count && name.find('\0') == std::string::npos; ++i) {
        uint32_t packed = words[cursor + i];
        for (int byte = 0; byte < 4; ++byte) {
          const char character = char(packed & 0xFFu);
          packed >>= 8;
          if (character == 0) {
            i = count;
            break;
          }
          name.push_back(character);
        }
      }
      seen_extensions.insert(name);
    } else if (opcode == 15 && count >= 2) {
      const uint32_t model = words[cursor + 1];
      const uint32_t expected = vertex ? 0u : 4u;
      if (model != expected) {
        Fail("spirv entry point stage does not match the shader");
      }
      ++entries;
    } else if (opcode == 71 && count >= 4) {
      const uint32_t id = words[cursor + 1];
      const uint32_t decoration = words[cursor + 2];
      if (decoration == 34) {
        set_by_id[id] = words[cursor + 3];
      } else if (decoration == 33) {
        binding_by_id[id] = words[cursor + 3];
      }
    }
    cursor += count;
  }
  if (entries != 1) {
    Fail("spirv module does not have one entry point");
  }
  for (uint32_t capability : seen_capabilities) {
    ++stats.capabilities[capability];
  }
  for (const std::string& extension : seen_extensions) {
    ++stats.extensions[extension];
  }
  for (const auto& set : set_by_id) {
    const auto binding = binding_by_id.find(set.first);
    if (binding == binding_by_id.end()) {
      continue;
    }
    seen_descriptors.insert((uint64_t(set.second) << 32) | binding->second);
  }
  for (uint64_t descriptor : seen_descriptors) {
    ++stats.descriptors[descriptor];
  }

  if (shader.memexport_eM_written() != 0) {
    ++stats.memexport;
  }
  if (shader.writes_depth()) {
    ++stats.depth;
  }
  const uint32_t textures = uint32_t(shader.GetTextureBindingsAfterTranslation().size());
  const uint32_t samplers = uint32_t(shader.GetSamplerBindingsAfterTranslation().size());
  const uint32_t floats = shader.constant_register_map().float_count;
  if (textures != 0) {
    ++stats.texture_shaders;
    stats.texture_max = std::max(stats.texture_max, textures);
  }
  if (samplers != 0) {
    ++stats.sampler_shaders;
    stats.sampler_max = std::max(stats.sampler_max, samplers);
  }
  if (vertex) {
    if (textures != 0) {
      ++stats.vertex_texture_shaders;
    }
    stats.vertex_texture_max = std::max(stats.vertex_texture_max, textures);
    stats.vertex_sampler_max = std::max(stats.vertex_sampler_max, samplers);
  } else {
    stats.pixel_texture_max = std::max(stats.pixel_texture_max, textures);
    stats.pixel_sampler_max = std::max(stats.pixel_sampler_max, samplers);
  }
  if (floats != 0) {
    ++stats.float_shaders;
    stats.float_max = std::max(stats.float_max, floats);
  }
}

uint32_t HostFromBe(uint32_t loaded) {
  return (loaded >> 24) | ((loaded >> 8) & 0x0000FF00u) | ((loaded << 8) & 0x00FF0000u) | (loaded << 24);
}

// This dump stores every vfetch with format 0 and stride 0. The translator then
// loads nothing. A single float4 fetch is enough to test the address path, so
// those empty fields become VertexFormat::k_32_32_32_32_FLOAT (38) and a stride
// of 4 dwords.
void PatchZeroVertexFetches(std::vector<uint32_t>& dwords) {
  if (dwords.size() < 3 || dwords.size() % 3 != 0) {
    return;
  }
  const uint32_t slots = static_cast<uint32_t>(dwords.size() / 3);
  uint32_t bound = slots;
  for (uint32_t slot = 0; slot < bound; ++slot) {
    rex::graphics::ucode::ControlFlowInstruction pair[2];
    rex::graphics::ucode::UnpackControlFlowInstructions(dwords.data() + slot * 3, pair);
    for (const rex::graphics::ucode::ControlFlowInstruction& instr : pair) {
      if (!rex::graphics::ucode::IsControlFlowOpcodeExec(instr.opcode())) {
        continue;
      }
      const uint32_t address = instr.exec.address();
      if (address != 0 && address < bound) {
        bound = address;
      }
    }
  }
  for (uint32_t slot = 0; slot < bound; ++slot) {
    rex::graphics::ucode::ControlFlowInstruction pair[2];
    rex::graphics::ucode::UnpackControlFlowInstructions(dwords.data() + slot * 3, pair);
    for (const rex::graphics::ucode::ControlFlowInstruction& instr : pair) {
      if (!rex::graphics::ucode::IsControlFlowOpcodeExec(instr.opcode())) {
        continue;
      }
      uint32_t sequence = instr.exec.sequence();
      const uint32_t address = instr.exec.address();
      const uint32_t count = instr.exec.count();
      for (uint32_t n = 0; n < count; ++n, sequence >>= 2) {
        if ((sequence & 1u) == 0) {
          continue;
        }
        const uint32_t at = (address + n) * 3;
        if (at + 2 >= dwords.size() || (dwords[at] & 0x1Fu) != 0) {
          continue;
        }
        if (((dwords[at + 1] >> 16) & 0x3Fu) == 0) {
          dwords[at + 1] |= 38u << 16;
        }
        if ((dwords[at + 2] & 0xFFu) == 0) {
          dwords[at + 2] |= 4u;
        }
      }
    }
  }
}

void TranslateShader(rex::graphics::SpirvShaderTranslator& translator, uint32_t token,
                     const UcodeSpan& ucode, TranslateStats& stats, HostProbe& probe,
                     const uint8_t* object, size_t object_size) {
  const auto type = token == 0xFFFE0300u ? rex::graphics::xenos::ShaderType::kVertex
                                         : rex::graphics::xenos::ShaderType::kPixel;
  rex::graphics::SpirvShader shader(type, 0, ucode.dwords, ucode.dword_count, std::endian::big);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  if (type == rex::graphics::xenos::ShaderType::kVertex) {
    const std::string_view text = disassembly.to_string_view();
    if (text.find("vfetch") != std::string_view::npos) {
      ++stats.disasm_vfetch;
    }
    if (text.find("DataFormat=") != std::string_view::npos) {
      ++stats.disasm_format;
    }
    if (text.find("Stride=") != std::string_view::npos) {
      ++stats.disasm_stride;
    }
    if (stats.disasm_sample.empty() && text.find("vfetch") != std::string_view::npos) {
      stats.disasm_sample.assign(text.data(), std::min<size_t>(text.size(), 2000));
      stats.sample_dword_count =
          static_cast<uint32_t>(std::min<size_t>(shader.ucode_dword_count(), 48));
      std::memcpy(stats.sample_dwords, shader.ucode_dwords(),
                  sizeof(uint32_t) * stats.sample_dword_count);
    }
  }
  if (shader.uses_register_dynamic_addressing()) {
    ++stats.dynamic_registers;
  }
  const uint64_t modification = type == rex::graphics::xenos::ShaderType::kVertex
                                    ? translator.GetDefaultVertexShaderModification(0)
                                    : translator.GetDefaultPixelShaderModification(0);
  rex::graphics::Shader::Translation& translation = *shader.GetOrCreateTranslation(modification);
  if (translator.TranslateAnalyzedShader(translation)) {
    const std::vector<uint8_t>& binary = translation.translated_binary();
    const bool spirv = binary.size() >= 4 && binary[0] == 0x03 && binary[1] == 0x02 &&
                       binary[2] == 0x23 && binary[3] == 0x07;
    if (spirv) {
      NoteModule(binary, type == rex::graphics::xenos::ShaderType::kVertex, shader, stats);
      HostProbe::ShaderStage stage;
      stage.vertex = type == rex::graphics::xenos::ShaderType::kVertex;
      stage.images = static_cast<uint32_t>(shader.GetTextureBindingsAfterTranslation().size());
      stage.samplers = static_cast<uint32_t>(shader.GetSamplerBindingsAfterTranslation().size());
      stage.color_mask = shader.writes_color_targets();
      stage.depth = shader.writes_depth();
      probe.Check(binary, stage);
      const bool guest_color = !stage.vertex && stage.color_mask == 1u && !stage.depth &&
                               !shader.kills_pixels() && binary.size() % 4 == 0;
      if (guest_color && stage.images == 0 && stage.samplers == 0 && stats.guest_alu.size() < 8) {
        TranslateStats::GuestDraw draw;
        draw.words.assign(reinterpret_cast<const uint32_t*>(binary.data()),
                          reinterpret_cast<const uint32_t*>(binary.data() + binary.size()));
        stats.guest_alu.push_back(std::move(draw));
      }
      bool only_2d = stage.images != 0;
      for (const rex::graphics::SpirvShader::TextureBinding& binding :
           shader.GetTextureBindingsAfterTranslation()) {
        if (binding.dimension != rex::graphics::xenos::FetchOpDimension::k2D) {
          only_2d = false;
        }
      }
      if (guest_color && only_2d && stage.images <= 8 && stage.samplers >= 1 &&
          stage.samplers <= 4) {
        ++stats.guest_textured_seen;
        if (stats.guest_textured.size() < 32) {
          TranslateStats::GuestDraw draw;
          draw.images = stage.images;
          draw.samplers = stage.samplers;
          draw.words.assign(reinterpret_cast<const uint32_t*>(binary.data()),
                            reinterpret_cast<const uint32_t*>(binary.data() + binary.size()));
          stats.guest_textured.push_back(std::move(draw));
        }
      }
      if (guest_color && stage.images == 0 && stage.samplers == 0 &&
          shader.register_static_address_bound() >= 1) {
        ++stats.guest_interpolated_seen;
        if (stats.guest_interpolated.size() < 16) {
          rex::graphics::SpirvShaderTranslator::Modification modification(
              translator.GetDefaultPixelShaderModification(0));
          modification.pixel.interpolator_mask = 1;
          rex::graphics::Shader::Translation& interpolated =
              *shader.GetOrCreateTranslation(modification.value);
          if (translator.TranslateAnalyzedShader(interpolated)) {
            const std::vector<uint8_t>& again = interpolated.translated_binary();
            if (again.size() >= 4 && again.size() % 4 == 0) {
              TranslateStats::GuestDraw draw;
              draw.interpolate = true;
              draw.words.assign(reinterpret_cast<const uint32_t*>(again.data()),
                                reinterpret_cast<const uint32_t*>(again.data() + again.size()));
              stats.guest_interpolated.push_back(std::move(draw));
            }
          }
        }
      }
      if (stage.vertex) {
        ++stats.vb_shaders;
        bool any_stride = false;
        bool any_float = false;
        bool any_index0 = false;
        if (!shader.vertex_bindings().empty()) {
          ++stats.vb_with_binding;
        }
        for (const rex::graphics::Shader::VertexBinding& binding : shader.vertex_bindings()) {
          for (const rex::graphics::Shader::VertexBinding::Attribute& attribute :
               binding.attributes) {
            const auto format_value = static_cast<uint32_t>(attribute.fetch_instr.attributes.data_format);
            if (format_value < 64) {
              ++stats.format_hits[format_value];
            }
            if (attribute.fetch_instr.attributes.stride != 0) {
              any_stride = true;
            }
            const rex::graphics::xenos::VertexFormat format =
                attribute.fetch_instr.attributes.data_format;
            if (format == rex::graphics::xenos::VertexFormat::k_32_FLOAT ||
                format == rex::graphics::xenos::VertexFormat::k_32_32_FLOAT ||
                format == rex::graphics::xenos::VertexFormat::k_32_32_32_FLOAT ||
                format == rex::graphics::xenos::VertexFormat::k_32_32_32_32_FLOAT) {
              any_float = true;
            }
            if (stats.example_format == 0xFFFFFFFFu && attribute.fetch_instr.operand_count > 0) {
              stats.example_format = format_value;
              stats.example_stride = attribute.fetch_instr.attributes.stride;
              stats.example_offset = attribute.fetch_instr.attributes.offset;
              stats.example_fetch = binding.fetch_constant;
              stats.example_index = attribute.fetch_instr.operands[0].storage_index;
              stats.example_swizzle = static_cast<uint32_t>(attribute.fetch_instr.operands[0].components[0]);
            }
            if (attribute.fetch_instr.operand_count > 0 &&
                attribute.fetch_instr.operands[0].storage_source ==
                    rex::graphics::InstructionStorageSource::kRegister &&
                attribute.fetch_instr.operands[0].storage_index == 0 &&
                !attribute.fetch_instr.operands[0].is_negated &&
                attribute.fetch_instr.operands[0].components[0] == rex::graphics::SwizzleSource::kX) {
              any_index0 = true;
            }
          }
        }
        if (any_stride) {
          ++stats.vb_attr_stride;
        }
        if (any_float) {
          ++stats.vb_float;
        }
        if (any_index0) {
          ++stats.vb_index0;
        }
      }
      if (stage.vertex && stage.images == 0 && stage.samplers == 0 &&
          (shader.writes_interpolators() & 1u) != 0 && shader.memexport_eM_written() == 0) {
        ++stats.guest_vertex_seen;
        const bool single_fetch =
            shader.vertex_bindings().size() == 1 && shader.vertex_bindings()[0].attributes.size() == 1 &&
            shader.vertex_bindings()[0].attributes[0].fetch_instr.operand_count > 0 &&
            shader.vertex_bindings()[0].attributes[0].fetch_instr.operands[0].storage_source ==
                rex::graphics::InstructionStorageSource::kRegister &&
            shader.vertex_bindings()[0].attributes[0].fetch_instr.operands[0].storage_index == 0 &&
            !shader.vertex_bindings()[0].attributes[0].fetch_instr.operands[0].is_negated &&
            shader.vertex_bindings()[0].attributes[0].fetch_instr.operands[0].components[0] ==
                rex::graphics::SwizzleSource::kX;
        if (single_fetch) {
          ++stats.guest_fetch_seen;
        }
        if (single_fetch && stats.guest_vertices.size() < 16) {
          std::vector<uint32_t> patched_ucode(ucode.dword_count);
          for (uint32_t i = 0; i < ucode.dword_count; ++i) {
            patched_ucode[i] = HostFromBe(ucode.dwords[i]);
          }
          PatchZeroVertexFetches(patched_ucode);
          rex::graphics::SpirvShader patched(rex::graphics::xenos::ShaderType::kVertex, 0,
                                            patched_ucode.data(), patched_ucode.size(),
                                            std::endian::native);
          rex::string::StringBuffer patched_disassembly;
          patched.AnalyzeUcode(patched_disassembly);
          rex::graphics::SpirvShaderTranslator::Modification modification(
              translator.GetDefaultVertexShaderModification(0));
          modification.vertex.interpolator_mask = 1;
          rex::graphics::Shader::Translation& vertex_out =
              *patched.GetOrCreateTranslation(modification.value);
          if (translator.TranslateAnalyzedShader(vertex_out)) {
            const std::vector<uint8_t>& again = vertex_out.translated_binary();
            if (again.size() >= 4 && again.size() % 4 == 0) {
              TranslateStats::GuestDraw draw;
              draw.vertex = true;
              draw.words.assign(reinterpret_cast<const uint32_t*>(again.data()),
                                reinterpret_cast<const uint32_t*>(again.data() + again.size()));
              for (const rex::graphics::Shader::VertexBinding& binding : patched.vertex_bindings()) {
                broken_bond::gfx::VertexStream stream;
                stream.fetch_constant = binding.fetch_constant;
                stream.stride_words = binding.stride_words;
                if (!binding.attributes.empty() && binding.attributes[0].fetch_instr.operand_count > 0 &&
                    binding.attributes[0].fetch_instr.operands[0].storage_source ==
                        rex::graphics::InstructionStorageSource::kRegister) {
                  stream.index_register = binding.attributes[0].fetch_instr.operands[0].storage_index;
                }
                for (const rex::graphics::Shader::VertexBinding::Attribute& attribute :
                     binding.attributes) {
                  if (stream.attribute_count >= 8) {
                    break;
                  }
                  const rex::graphics::xenos::VertexFormat format =
                      attribute.fetch_instr.attributes.data_format;
                  broken_bond::gfx::VertexAttribute& out =
                      stream.attributes[stream.attribute_count++];
                  out.offset_words = attribute.fetch_instr.attributes.offset;
                  out.is_float = format == rex::graphics::xenos::VertexFormat::k_32_FLOAT ||
                                 format == rex::graphics::xenos::VertexFormat::k_32_32_FLOAT ||
                                 format == rex::graphics::xenos::VertexFormat::k_32_32_32_FLOAT ||
                                 format == rex::graphics::xenos::VertexFormat::k_32_32_32_32_FLOAT;
                  out.components =
                      out.is_float
                          ? uint32_t(rex::graphics::xenos::GetVertexFormatComponentCount(format))
                          : 0;
                  if (attribute.fetch_instr.attributes.stride > stream.stride_words) {
                    stream.stride_words = attribute.fetch_instr.attributes.stride;
                  }
                }
                draw.streams.push_back(stream);
              }
              stats.guest_vertices.push_back(std::move(draw));
            }
          }
        }
      }
#if defined(BROKEN_BOND_HOST_GFX)
      if (stage.vertex && stage.images == 0 && stage.samplers == 0 &&
          (shader.writes_interpolators() & 1u) != 0 && shader.memexport_eM_written() == 0 &&
          stats.guest_declared.size() < 8 && object != nullptr) {
        bool one_stream = !shader.vertex_bindings().empty();
        uint32_t fetch_constant = 0;
        bool fetch_set = false;
        for (const rex::graphics::Shader::VertexBinding& binding : shader.vertex_bindings()) {
          if (!fetch_set) {
            fetch_constant = binding.fetch_constant;
            fetch_set = true;
          } else if (binding.fetch_constant != fetch_constant) {
            one_stream = false;
          }
          for (const rex::graphics::Shader::VertexBinding::Attribute& attribute : binding.attributes) {
            if (attribute.fetch_instr.operand_count == 0) {
              one_stream = false;
              continue;
            }
            const rex::graphics::InstructionOperand& index = attribute.fetch_instr.operands[0];
            if (index.storage_source != rex::graphics::InstructionStorageSource::kRegister ||
                index.storage_index != 0 || index.is_negated ||
                index.components[0] != rex::graphics::SwizzleSource::kX) {
              one_stream = false;
            }
          }
        }
        std::vector<broken_bond::gfx::VertexFetchSemantic> semantics;
        std::string reason;
        if (one_stream &&
            broken_bond::gfx::ReadVertexFetchSemantics(object, object_size, semantics, reason)) {
          bool position = false;
          bool other = false;
          for (const broken_bond::gfx::VertexFetchSemantic& semantic : semantics) {
            if (semantic.usage == 0 && semantic.usage_index == 0) {
              position = true;
            } else {
              other = true;
            }
          }
          std::vector<broken_bond::gfx::VertexElement> elements;
          if (position && other) {
            for (const broken_bond::gfx::VertexFetchSemantic& semantic : semantics) {
              bool seen = false;
              for (const broken_bond::gfx::VertexElement& element : elements) {
                if (element.usage == semantic.usage && element.usage_index == semantic.usage_index) {
                  seen = true;
                }
              }
              if (seen) {
                continue;
              }
              broken_bond::gfx::VertexElement element;
              element.usage = semantic.usage;
              element.usage_index = semantic.usage_index;
              element.format = 38;
              element.stride_words = 8;
              element.offset_words = semantic.usage == 0 && semantic.usage_index == 0 ? 0 : 4;
              elements.push_back(element);
            }
          }
          std::vector<uint32_t> patched_ucode(ucode.dword_count);
          for (uint32_t i = 0; i < ucode.dword_count; ++i) {
            patched_ucode[i] = HostFromBe(ucode.dwords[i]);
          }
          if (position && other &&
              broken_bond::gfx::PatchVertexDeclaration(patched_ucode.data(),
                                                       uint32_t(patched_ucode.size()), semantics.data(),
                                                       uint32_t(semantics.size()), elements.data(),
                                                       uint32_t(elements.size()), reason)) {
            rex::graphics::SpirvShader patched(rex::graphics::xenos::ShaderType::kVertex, 0,
                                              patched_ucode.data(), patched_ucode.size(),
                                              std::endian::native);
            rex::string::StringBuffer patched_disassembly;
            patched.AnalyzeUcode(patched_disassembly);
            rex::graphics::SpirvShaderTranslator::Modification modification(
                translator.GetDefaultVertexShaderModification(0));
            modification.vertex.interpolator_mask = 1;
            rex::graphics::Shader::Translation& declared =
                *patched.GetOrCreateTranslation(modification.value);
            if (translator.TranslateAnalyzedShader(declared)) {
              const std::vector<uint8_t>& again = declared.translated_binary();
              if (again.size() >= 4 && again.size() % 4 == 0) {
                TranslateStats::GuestDraw draw;
                draw.vertex = true;
                draw.declare_test = true;
                draw.words.assign(reinterpret_cast<const uint32_t*>(again.data()),
                                  reinterpret_cast<const uint32_t*>(again.data() + again.size()));
                broken_bond::gfx::VertexStream stream;
                stream.fetch_constant = fetch_constant;
                stream.stride_words = 8;
                stream.index_register = 0;
                stream.attribute_count = 2;
                stream.attributes[0].offset_words = 0;
                stream.attributes[0].components = 4;
                stream.attributes[0].is_float = true;
                stream.attributes[1].offset_words = 4;
                stream.attributes[1].components = 4;
                stream.attributes[1].is_float = true;
                draw.streams.push_back(stream);
                stats.guest_declared.push_back(std::move(draw));
              }
            }
          }
          if (position && other && stats.guest_packed.size() < 8) {
            struct PackedSemantic {
              uint32_t usage;
              uint32_t usage_index;
              uint32_t rank;
            };
            std::vector<PackedSemantic> packed_semantics;
            for (const broken_bond::gfx::VertexFetchSemantic& semantic : semantics) {
              bool seen = false;
              for (const PackedSemantic& item : packed_semantics) {
                if (item.usage == semantic.usage && item.usage_index == semantic.usage_index) {
                  seen = true;
                }
              }
              if (seen) {
                continue;
              }
              uint32_t rank = 1000 + semantic.usage * 16 + semantic.usage_index;
              if (semantic.usage == 0) {
                rank = semantic.usage_index;
              } else if (semantic.usage == 10) {
                rank = 10 + semantic.usage_index;
              } else if (semantic.usage == 2) {
                rank = 20 + semantic.usage_index;
              } else if (semantic.usage == 1) {
                rank = 30 + semantic.usage_index;
              } else if (semantic.usage == 3) {
                rank = 40 + semantic.usage_index;
              } else if (semantic.usage == 6) {
                rank = 50 + semantic.usage_index;
              } else if (semantic.usage == 5) {
                rank = 70 + semantic.usage_index;
              }
              packed_semantics.push_back({semantic.usage, semantic.usage_index, rank});
            }
            std::sort(packed_semantics.begin(), packed_semantics.end(),
                      [](const PackedSemantic& a, const PackedSemantic& b) { return a.rank < b.rank; });
            if (packed_semantics.size() <= 8 && packed_semantics[0].usage == 0 &&
                packed_semantics[0].usage_index == 0) {
              std::vector<broken_bond::gfx::VertexElement> packed_elements;
              int32_t cursor = 0;
              for (const PackedSemantic& item : packed_semantics) {
                broken_bond::gfx::VertexElement element;
                element.usage = item.usage;
                element.usage_index = item.usage_index;
                element.offset_words = cursor;
                if (item.usage == 0 && item.usage_index == 0) {
                  element.format = 26;
                  element.signed_components = true;
                  element.integer_components = true;
                  cursor += 2;
                } else {
                  element.format = 38;
                  cursor += 4;
                }
                packed_elements.push_back(element);
              }
              for (broken_bond::gfx::VertexElement& element : packed_elements) {
                element.stride_words = static_cast<uint32_t>(cursor);
              }
              std::vector<uint32_t> packed_ucode(ucode.dword_count);
              for (uint32_t i = 0; i < ucode.dword_count; ++i) {
                packed_ucode[i] = HostFromBe(ucode.dwords[i]);
              }
              if (broken_bond::gfx::PatchVertexDeclaration(
                      packed_ucode.data(), uint32_t(packed_ucode.size()), semantics.data(),
                      uint32_t(semantics.size()), packed_elements.data(),
                      uint32_t(packed_elements.size()), reason)) {
                rex::graphics::SpirvShader patched(rex::graphics::xenos::ShaderType::kVertex, 0,
                                                  packed_ucode.data(), packed_ucode.size(),
                                                  std::endian::native);
                rex::string::StringBuffer patched_disassembly;
                patched.AnalyzeUcode(patched_disassembly);
                rex::graphics::SpirvShaderTranslator::Modification modification(
                    translator.GetDefaultVertexShaderModification(0));
                modification.vertex.interpolator_mask = shader.writes_interpolators() & 0xFFFFu;
                rex::graphics::Shader::Translation& packed_translation =
                    *patched.GetOrCreateTranslation(modification.value);
                if (translator.TranslateAnalyzedShader(packed_translation)) {
                  const std::vector<uint8_t>& again = packed_translation.translated_binary();
                  if (again.size() >= 4 && again.size() % 4 == 0) {
                    TranslateStats::GuestDraw draw;
                    draw.vertex = true;
                    draw.declare_test = true;
                    draw.words.assign(reinterpret_cast<const uint32_t*>(again.data()),
                                      reinterpret_cast<const uint32_t*>(again.data() + again.size()));
                    broken_bond::gfx::VertexStream stream;
                    stream.fetch_constant = fetch_constant;
                    stream.stride_words = static_cast<uint32_t>(cursor);
                    stream.index_register = 0;
                    stream.attribute_count = static_cast<uint32_t>(packed_elements.size());
                    for (uint32_t i = 0; i < stream.attribute_count; ++i) {
                      stream.attributes[i].offset_words = packed_elements[i].offset_words;
                      stream.attributes[i].components = 4;
                      stream.attributes[i].short4 = packed_elements[i].format == 26;
                      stream.attributes[i].is_float = packed_elements[i].format == 38;
                      stream.attributes[i].usage = packed_elements[i].usage;
                      stream.attributes[i].usage_index = packed_elements[i].usage_index;
                    }
                    draw.streams.push_back(stream);
                    draw.interpolators = shader.writes_interpolators();
                    stats.guest_packed.push_back(std::move(draw));
                  }
                }
              }
            }
          }
        }
      }
      if (stage.vertex && stage.images == 0 && stage.samplers == 0 &&
          (shader.writes_interpolators() & 1u) != 0 && shader.memexport_eM_written() == 0 &&
          shader.vertex_bindings().size() == 1 && shader.vertex_bindings()[0].attributes.size() == 1 &&
          stats.guest_short.size() < 8 && object != nullptr) {
        const rex::graphics::Shader::VertexBinding::Attribute& attribute =
            shader.vertex_bindings()[0].attributes[0];
        const bool index_x = attribute.fetch_instr.operand_count > 0 &&
                             attribute.fetch_instr.operands[0].storage_source ==
                                 rex::graphics::InstructionStorageSource::kRegister &&
                             attribute.fetch_instr.operands[0].storage_index == 0 &&
                             !attribute.fetch_instr.operands[0].is_negated &&
                             attribute.fetch_instr.operands[0].components[0] ==
                                 rex::graphics::SwizzleSource::kX;
        std::vector<broken_bond::gfx::VertexFetchSemantic> semantics;
        std::string reason;
        if (index_x &&
            broken_bond::gfx::ReadVertexFetchSemantics(object, object_size, semantics, reason) &&
            semantics.size() == 1 && semantics[0].usage == 0 && semantics[0].usage_index == 0) {
          broken_bond::gfx::VertexElement element;
          element.usage = 0;
          element.format = 26;
          element.offset_words = 0;
          element.stride_words = 2;
          element.signed_components = true;
          element.integer_components = true;
          std::vector<uint32_t> patched_ucode(ucode.dword_count);
          for (uint32_t i = 0; i < ucode.dword_count; ++i) {
            patched_ucode[i] = HostFromBe(ucode.dwords[i]);
          }
          if (broken_bond::gfx::PatchVertexDeclaration(
                  patched_ucode.data(), uint32_t(patched_ucode.size()), semantics.data(), 1,
                  &element, 1, reason)) {
            rex::graphics::SpirvShader patched(rex::graphics::xenos::ShaderType::kVertex, 0,
                                              patched_ucode.data(), patched_ucode.size(),
                                              std::endian::native);
            rex::string::StringBuffer patched_disassembly;
            patched.AnalyzeUcode(patched_disassembly);
            rex::graphics::SpirvShaderTranslator::Modification modification(
                translator.GetDefaultVertexShaderModification(0));
            modification.vertex.interpolator_mask = 1;
            rex::graphics::Shader::Translation& shortened =
                *patched.GetOrCreateTranslation(modification.value);
            if (translator.TranslateAnalyzedShader(shortened)) {
              const std::vector<uint8_t>& again = shortened.translated_binary();
              if (again.size() >= 4 && again.size() % 4 == 0) {
                TranslateStats::GuestDraw draw;
                draw.vertex = true;
                draw.words.assign(reinterpret_cast<const uint32_t*>(again.data()),
                                  reinterpret_cast<const uint32_t*>(again.data() + again.size()));
                broken_bond::gfx::VertexStream stream;
                stream.fetch_constant = shader.vertex_bindings()[0].fetch_constant;
                stream.stride_words = 2;
                stream.index_register = 0;
                stream.attribute_count = 1;
                stream.attributes[0].offset_words = 0;
                stream.attributes[0].components = 4;
                stream.attributes[0].short4 = true;
                stream.short_scale = 2;
                stream.short_w_scale = 1;
                draw.streams.push_back(stream);
                stats.guest_short.push_back(std::move(draw));
              }
            }
          }
        }
      }
#endif
      ++stats.ok;
      stats.spirv_bytes += binary.size();
      return;
    }
    ++stats.failed;
    ++stats.fatals["translated module is not SPIR-V"];
    return;
  }
  ++stats.failed;
  for (const rex::graphics::Shader::Error& error : translation.errors()) {
    if (error.is_fatal) {
      ++stats.fatals[error.message];
    }
  }
}

#if defined(BROKEN_BOND_HOST_GFX)
int DrawBootPair(const std::vector<uint8_t>& file, const std::vector<ShaderRecord>& shaders) {
  if (shaders.size() <= 2775u) {
    Fail("shader database does not contain the boot pair");
  }
  const std::vector<uint8_t> vertex_object = InflateZlib(
      file.data() + shaders[1].offset, shaders[1].compressed_size, shaders[1].raw_size);
  const std::vector<uint8_t> pixel_object = InflateZlib(
      file.data() + shaders[2775].offset, shaders[2775].compressed_size, shaders[2775].raw_size);
  std::vector<broken_bond::gfx::VertexFetchSemantic> semantics;
  std::string reason;
  if (!broken_bond::gfx::ReadVertexFetchSemantics(vertex_object.data(), vertex_object.size(),
                                                 semantics, reason)) {
    std::cerr << reason << "\n";
    return 1;
  }
  std::cout << "boot_pair fetches " << semantics.size() << "\n";
  for (const broken_bond::gfx::VertexFetchSemantic& semantic : semantics) {
    std::cout << "  fetch " << semantic.instruction << " "
              << broken_bond::gfx::VertexUsageName(semantic.usage) << semantic.usage_index << "\n";
  }
  const UcodeSpan vertex_ucode =
      LocateUcode(vertex_object.data(), vertex_object.size(), 0xFFFE0300u);
  const UcodeSpan pixel_ucode = LocateUcode(pixel_object.data(), pixel_object.size(), 0xFFFF0300u);
  broken_bond::gfx::VertexElement elements[2];
  elements[0].usage = 0;
  // The translator pads a 3-float fetch with 0. This shader divides by that
  // w, and a 3-component fetch on the GPU supplies 1, so the draw loads a
  // fourth float of 1.
  elements[0].format = 38;
  elements[0].offset_words = 0;
  elements[0].stride_words = 5;
  elements[1].usage = 5;
  elements[1].format = 37;
  elements[1].offset_words = 3;
  elements[1].stride_words = 5;

  HostProbe probe;
  probe.Open();
  rex::graphics::SpirvShaderTranslator::Features features(false);
  if (probe.create_module != nullptr && probe.api >= VK_API_VERSION_1_2) {
    features.spirv_version = spv::Spv_1_5;
    features.max_storage_buffer_range = probe.storage_range;
    features.signed_zero_inf_nan_preserve_float32 = probe.preserve;
    features.denorm_flush_to_zero_float32 = probe.flush;
    features.rounding_mode_rte_float32 = probe.rte;
    features.demote_to_helper_invocation = probe.demote;
    features.sample_rate_shading = probe.sample_rate;
  } else {
    features = rex::graphics::SpirvShaderTranslator::Features(true);
  }
  rex::graphics::SpirvShaderTranslator translator(features, false, false, false);

  auto translate = [&](bool vertex, uint32_t mask, rex::graphics::SpirvShader& shader,
                       std::vector<uint32_t>& words) -> bool {
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    const uint64_t modification_value =
        vertex ? translator.GetDefaultVertexShaderModification(0)
               : translator.GetDefaultPixelShaderModification(0);
    rex::graphics::SpirvShaderTranslator::Modification modification(modification_value);
    if (vertex) {
      modification.vertex.interpolator_mask = mask;
    } else {
      modification.pixel.interpolator_mask = mask;
    }
    rex::graphics::Shader::Translation& translation =
        *shader.GetOrCreateTranslation(modification.value);
    if (!translator.TranslateAnalyzedShader(translation)) {
      reason = "boot pair shader did not translate";
      return false;
    }
    const std::vector<uint8_t>& binary = translation.translated_binary();
    if (binary.size() < 4 || binary.size() % 4 != 0) {
      reason = "boot pair shader is not a SPIR-V module";
      return false;
    }
    words.assign(reinterpret_cast<const uint32_t*>(binary.data()),
                 reinterpret_cast<const uint32_t*>(binary.data() + binary.size()));
    return true;
  };

  std::vector<uint32_t> vertex_words;
  std::vector<uint32_t> pixel_words;
  std::vector<uint32_t> patched(vertex_ucode.dword_count);
  for (uint32_t i = 0; i < vertex_ucode.dword_count; ++i) {
    patched[i] = HostFromBe(vertex_ucode.dwords[i]);
  }
  if (!broken_bond::gfx::PatchVertexDeclaration(patched.data(), uint32_t(patched.size()),
                                               semantics.data(), uint32_t(semantics.size()),
                                               elements, 2, reason)) {
    std::cerr << reason << "\n";
    return 1;
  }
  rex::graphics::SpirvShader vertex_shader(rex::graphics::xenos::ShaderType::kVertex, 0,
                                          patched.data(), uint32_t(patched.size()),
                                          std::endian::native);
  rex::string::StringBuffer vertex_disassembly;
  vertex_shader.AnalyzeUcode(vertex_disassembly);
  const uint32_t mask = vertex_shader.writes_interpolators() & 0xFFFFu;
  if (!translate(true, mask, vertex_shader, vertex_words)) {
    std::cerr << reason << "\n";
    return 1;
  }
  rex::graphics::SpirvShader pixel_shader(rex::graphics::xenos::ShaderType::kPixel, 0,
                                         pixel_ucode.dwords, pixel_ucode.dword_count,
                                         std::endian::big);
  rex::string::StringBuffer pixel_disassembly;
  pixel_shader.AnalyzeUcode(pixel_disassembly);
  if (!translate(false, mask, pixel_shader, pixel_words)) {
    std::cerr << reason << "\n";
    return 1;
  }
  const uint32_t image_count =
      uint32_t(pixel_shader.GetTextureBindingsAfterTranslation().size());
  const uint32_t sampler_count =
      uint32_t(pixel_shader.GetSamplerBindingsAfterTranslation().size());
  if (vertex_shader.vertex_bindings().empty()) {
    std::cerr << "boot pair vertex shader has no fetch\n";
    return 1;
  }
  broken_bond::gfx::VertexStream stream;
  stream.fetch_constant = vertex_shader.vertex_bindings()[0].fetch_constant;
  stream.stride_words = 5;
  for (const rex::graphics::Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    for (const rex::graphics::Shader::VertexBinding::Attribute& attribute : binding.attributes) {
      if (stream.attribute_count >= 8) {
        break;
      }
      const rex::graphics::xenos::VertexFormat format =
          attribute.fetch_instr.attributes.data_format;
      broken_bond::gfx::VertexAttribute& out = stream.attributes[stream.attribute_count];
      out.offset_words = attribute.fetch_instr.attributes.offset;
      out.is_float = format == rex::graphics::xenos::VertexFormat::k_32_32_FLOAT ||
                     format == rex::graphics::xenos::VertexFormat::k_32_32_32_FLOAT ||
                     format == rex::graphics::xenos::VertexFormat::k_32_32_32_32_FLOAT;
      out.components = out.is_float
                           ? uint32_t(rex::graphics::xenos::GetVertexFormatComponentCount(format))
                           : 0;
      if (out.offset_words == 0) {
        stream.triangle_attribute = stream.attribute_count;
      } else {
        out.constant_fill = true;
      }
      ++stream.attribute_count;
    }
  }
  const float supplied[3][5] = {
      {-1.f, -1.f, 0.f, 1.f, 0.5f},
      {3.f, -1.f, 0.f, 1.f, 0.5f},
      {-1.f, 3.f, 0.f, 1.f, 0.5f},
  };
  stream.bytes = reinterpret_cast<const uint8_t*>(supplied);
  stream.byte_count = sizeof(supplied);
  broken_bond::gfx::Device device;
  if (!device.Open(reason)) {
    std::cerr << reason << "\n";
    return 1;
  }
  constexpr uint32_t kWidth = 1280;
  constexpr uint32_t kHeight = 512;
  std::vector<uint8_t> pixels;
  if (!device.DrawGuestFragment(vertex_words.data(), vertex_words.size(), kWidth, kHeight,
                                image_count, sampler_count, 1, false, 0.f, true, &stream, 1,
                                pixels, reason, pixel_words.data(), pixel_words.size())) {
    std::cerr << reason << "\n";
    return 1;
  }
  uint32_t matched = 0;
  int other[4] = {0, 0, 0, 0};
  bool saw_other = false;
  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    if (pixels[i] == 255 && pixels[i + 1] == 10 && pixels[i + 2] == 10 && pixels[i + 3] == 255) {
      ++matched;
      continue;
    }
    if (!saw_other) {
      saw_other = true;
      other[0] = pixels[i];
      other[1] = pixels[i + 1];
      other[2] = pixels[i + 2];
      other[3] = pixels[i + 3];
    }
  }
  std::cout << "boot_pair mask " << mask << " images " << image_count << " samplers "
            << sampler_count << " matched " << matched << " other " << other[0] << " " << other[1]
            << " " << other[2] << " " << other[3] << "\n";
  if (matched != kWidth * kHeight) {
    const std::string_view vertex_text = vertex_disassembly.to_string_view();
    const std::string_view pixel_text = pixel_disassembly.to_string_view();
    std::cout << std::string(vertex_text.data(), vertex_text.size()) << "\n";
    std::cout << std::string(pixel_text.data(), pixel_text.size()) << "\n";
    std::cout << "fetch " << stream.fetch_constant << " attrs " << stream.attribute_count << "\n";
    return 1;
  }
  std::cout << "boot_pair_ok 1\n";
  return 0;
}

void StoreBe(std::vector<uint8_t>& image, uint32_t at, uint32_t value) {
  image[at] = static_cast<uint8_t>(value >> 24);
  image[at + 1] = static_cast<uint8_t>(value >> 16);
  image[at + 2] = static_cast<uint8_t>(value >> 8);
  image[at + 3] = static_cast<uint8_t>(value);
}

uint32_t LoadBe(const std::vector<uint8_t>& image, uint32_t at) {
  return (uint32_t(image[at]) << 24) | (uint32_t(image[at + 1]) << 16) |
         (uint32_t(image[at + 2]) << 8) | uint32_t(image[at + 3]);
}

int DrawMesh(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Fail(std::string("cannot open ") + path);
  }
  uint32_t magic = 0;
  uint32_t image_device = 0;
  uint32_t image_bytes = 0;
  if (!in.read(reinterpret_cast<char*>(&magic), 4) ||
      !in.read(reinterpret_cast<char*>(&image_device), 4) ||
      !in.read(reinterpret_cast<char*>(&image_bytes), 4) || magic != 0x31445242u ||
      image_bytes > (20u << 20)) {
    Fail("mesh snapshot is not a device draw");
  }
  std::vector<uint8_t> image(image_bytes);
  if (image_bytes != 0 &&
      !in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(image_bytes))) {
    Fail("mesh snapshot is short");
  }
  std::string reason;
  broken_bond::gfx::DeviceDraw draw;
  std::vector<uint8_t> stream_bytes;
  std::vector<broken_bond::gfx::VertexElement> elements;
  broken_bond::gfx::ShaderMicrocode vertex_ucode;
  broken_bond::gfx::ShaderMicrocode pixel_ucode;
  std::vector<uint8_t> vertex_raw;
  std::vector<uint8_t> pixel_raw;
  std::vector<float> floats;
  if (!broken_bond::gfx::ReadDeviceDraw(image.data(), image.size(), image_device, draw, stream_bytes,
                                        reason) ||
      !broken_bond::gfx::ReadDeviceDeclaration(image.data(), image.size(), image_device, elements,
                                               reason) ||
      !broken_bond::gfx::ReadVertexShaderMicrocode(image.data(), image.size(), draw.vertex_shader,
                                                   vertex_ucode, vertex_raw, reason) ||
      !broken_bond::gfx::ReadPixelShaderMicrocode(image.data(), image.size(), draw.pixel_shader,
                                                  pixel_ucode, pixel_raw, reason) ||
      !broken_bond::gfx::ReadDeviceFloats(image.data(), image.size(), image_device, floats,
                                          reason)) {
    Fail(reason.empty() ? "mesh snapshot was not read" : reason);
  }
  if (image.size() < 12456u || vertex_raw.size() < 12 || vertex_raw.size() % 4 != 0 ||
      pixel_raw.size() < 12 || floats.size() < 2048u || elements.empty() ||
      draw.stream_stride_dwords == 0) {
    Fail("mesh snapshot is missing a shader, a declaration, or vertices");
  }
  const uint32_t index_at = LoadBe(image, 12436u);
  const uint32_t index_bytes = LoadBe(image, 12440u);
  const uint32_t index_size = LoadBe(image, 12444u);
  const uint32_t index_first = LoadBe(image, 12448u);
  if (index_size != 2u || index_bytes < 6u || index_bytes % 2u != 0 ||
      uint64_t(index_at) + index_bytes > image.size()) {
    Fail("mesh snapshot index list is not 16-bit");
  }
  std::vector<uint16_t> indices(index_bytes / 2u);
  for (uint32_t i = 0; i < indices.size(); ++i) {
    const uint32_t at = index_at + i * 2u;
    uint16_t index = uint16_t((uint32_t(image[at]) << 8) | uint32_t(image[at + 1]));
    indices[i] = index >= index_first ? uint16_t(index - index_first) : index;
  }
  const uint32_t vertex_object = draw.vertex_shader & 0x1FFFFFFFu;
  int marker = -1;
  if (vertex_object + 0x800u <= image.size()) {
    for (uint32_t at = vertex_object; at + 4u <= vertex_object + 0x800u; at += 4u) {
      if (LoadBe(image, at) == 0x290u) {
        marker = static_cast<int>(at);
        break;
      }
    }
  }
  if (marker < 0) {
    Fail("mesh vertex shader has no semantic marker");
  }
  std::vector<uint8_t> container(0x100u + vertex_raw.size(), 0);
  StoreBe(container, 4u, 0x100u);
  StoreBe(container, 24u, 0x60u);
  StoreBe(container, 48u, 0xFFFE0300u);
  StoreBe(container, 0x60u, 0u);
  StoreBe(container, 0x64u, uint32_t(vertex_raw.size()));
  const uint32_t blob = std::min(32u, uint32_t(image.size() - uint32_t(marker)) / 4u);
  for (uint32_t i = 0; i < blob; ++i) {
    StoreBe(container, 0x68u + i * 4u, LoadBe(image, uint32_t(marker) + i * 4u));
  }
  std::memcpy(container.data() + 0x100u, vertex_raw.data(), vertex_raw.size());
  std::vector<broken_bond::gfx::VertexFetchSemantic> semantics;
  if (!broken_bond::gfx::ReadVertexFetchSemantics(container.data(), container.size(), semantics,
                                                 reason)) {
    Fail(reason.empty() ? "mesh vertex semantics were not read" : reason);
  }
  std::vector<uint32_t> patched(vertex_raw.size() / 4u);
  for (uint32_t i = 0; i < patched.size(); ++i) {
    uint32_t loaded = 0;
    std::memcpy(&loaded, vertex_raw.data() + i * 4u, 4u);
    patched[i] = HostFromBe(loaded);
  }
  if (!broken_bond::gfx::PatchVertexDeclaration(patched.data(), uint32_t(patched.size()),
                                               semantics.data(), uint32_t(semantics.size()),
                                               elements.data(), uint32_t(elements.size()),
                                               reason)) {
    Fail(reason.empty() ? "mesh declaration was not patched" : reason);
  }
  HostProbe probe;
  probe.Open();
  rex::graphics::SpirvShaderTranslator::Features features(false);
  if (probe.create_module != nullptr && probe.api >= VK_API_VERSION_1_2) {
    features.spirv_version = spv::Spv_1_5;
    features.max_storage_buffer_range = probe.storage_range;
    features.signed_zero_inf_nan_preserve_float32 = probe.preserve;
    features.denorm_flush_to_zero_float32 = probe.flush;
    features.rounding_mode_rte_float32 = probe.rte;
    features.demote_to_helper_invocation = probe.demote;
    features.sample_rate_shading = probe.sample_rate;
  } else {
    features = rex::graphics::SpirvShaderTranslator::Features(true);
  }
  rex::graphics::SpirvShaderTranslator translator(features, false, false, false);
  auto translate = [&](bool vertex, uint32_t mask, rex::graphics::SpirvShader& shader,
                       std::vector<uint32_t>& words) -> bool {
    rex::string::StringBuffer disassembly;
    shader.AnalyzeUcode(disassembly);
    const uint64_t modification_value = vertex ? translator.GetDefaultVertexShaderModification(0)
                                              : translator.GetDefaultPixelShaderModification(0);
    rex::graphics::SpirvShaderTranslator::Modification modification(modification_value);
    if (vertex) {
      modification.vertex.interpolator_mask = mask;
    } else {
      modification.pixel.interpolator_mask = mask;
    }
    rex::graphics::Shader::Translation& translation =
        *shader.GetOrCreateTranslation(modification.value);
    if (!translator.TranslateAnalyzedShader(translation)) {
      reason = "mesh shader did not translate";
      return false;
    }
    const std::vector<uint8_t>& binary = translation.translated_binary();
    if (binary.size() < 4 || binary.size() % 4 != 0) {
      reason = "mesh shader is not a SPIR-V module";
      return false;
    }
    words.assign(reinterpret_cast<const uint32_t*>(binary.data()),
                 reinterpret_cast<const uint32_t*>(binary.data() + binary.size()));
    return true;
  };
  rex::graphics::SpirvShader vertex_shader(rex::graphics::xenos::ShaderType::kVertex, 0,
                                          patched.data(), uint32_t(patched.size()),
                                          std::endian::native);
  rex::string::StringBuffer vertex_disassembly;
  vertex_shader.AnalyzeUcode(vertex_disassembly);
  const uint32_t mask = vertex_shader.writes_interpolators() & 0xFFFFu;
  std::vector<uint32_t> vertex_words;
  std::vector<uint32_t> pixel_words;
  if (!translate(true, mask, vertex_shader, vertex_words)) {
    Fail(reason);
  }
  rex::graphics::SpirvShader pixel_shader(rex::graphics::xenos::ShaderType::kPixel, 0,
                                         reinterpret_cast<const uint32_t*>(pixel_raw.data()),
                                         uint32_t(pixel_raw.size() / 4u), std::endian::big);
  if (!translate(false, mask, pixel_shader, pixel_words)) {
    Fail(reason);
  }
  if (vertex_shader.vertex_bindings().empty()) {
    Fail("mesh vertex shader has no fetch");
  }
  const uint32_t image_count = uint32_t(pixel_shader.GetTextureBindingsAfterTranslation().size());
  const uint32_t sampler_count = uint32_t(pixel_shader.GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_offset = 0;
  uint32_t texture_bytes = 0;
  uint32_t texture_format = 0;
  uint32_t texture_width_px = 0;
  uint32_t texture_height_px = 0;
  uint32_t texture_pitch = 0;
  uint32_t texture_endian = 0;
  uint32_t texture_tiled = 0;
  if (image.size() >= 12488u) {
    texture_offset = LoadBe(image, 12456u);
    texture_bytes = LoadBe(image, 12460u);
    texture_format = LoadBe(image, 12464u);
    texture_width_px = LoadBe(image, 12468u);
    texture_height_px = LoadBe(image, 12472u);
    texture_pitch = LoadBe(image, 12476u);
    texture_endian = LoadBe(image, 12480u);
    texture_tiled = LoadBe(image, 12484u);
  }
  const bool texture_tail = texture_bytes != 0 && texture_offset <= image.size() &&
                            texture_bytes <= image.size() - texture_offset &&
                            texture_offset + texture_bytes == image.size() &&
                            (texture_format == 6u || texture_format == 18u);
  if (!texture_tail) {
    texture_bytes = 0;
  }
  std::vector<uint8_t> rgba;
  if (texture_bytes != 0 &&
      !broken_bond::gfx::LoadGuestTextureRgba8(
          texture_format, texture_width_px, texture_height_px, texture_pitch, texture_endian,
          texture_tiled != 0, image.data() + texture_offset, texture_bytes, rgba, reason)) {
    Fail(reason.empty() ? "mesh texture was not decoded" : reason);
  }
  broken_bond::gfx::Device device;
  if (!device.Open(reason)) {
    Fail(reason);
  }
  std::cout << "mesh verts " << (stream_bytes.size() / (draw.stream_stride_dwords * 4u))
            << " indices " << indices.size() << " vs " << vertex_raw.size() << " ps "
            << pixel_raw.size() << " decl " << elements.size() << " fetches " << semantics.size()
            << " mask " << mask << " images " << image_count << " vf "
            << vertex_shader.vertex_bindings()[0].fetch_constant << "\n";
  for (const auto& binding : pixel_shader.GetTextureBindingsAfterTranslation()) {
    std::cout << "mesh tf " << binding.fetch_constant << "\n";
  }
  if (texture_bytes == 0) {
    std::cout << "mesh texture 0\n";
  } else {
    std::cout << "mesh texture " << texture_width_px << "x" << texture_height_px << " format "
              << texture_format << " endian " << texture_endian << " tiled " << texture_tiled
              << " bytes " << texture_bytes << " texel " << int(rgba[0]) << " " << int(rgba[1])
              << " " << int(rgba[2]) << " " << int(rgba[3]) << "\n";
  }
  constexpr uint32_t kWidth = 1280;
  constexpr uint32_t kHeight = 512;
  const uint8_t* texture_rgba = rgba.empty() ? nullptr : rgba.data();
  const uint32_t texture_width = rgba.empty() ? 0u : texture_width_px;
  const uint32_t texture_height = rgba.empty() ? 0u : texture_height_px;
  auto draw_mesh = [&](uint32_t endian, const broken_bond::gfx::Device::RasterState* raster,
                       const char* label) {
    broken_bond::gfx::VertexStream stream;
    stream.fetch_constant = vertex_shader.vertex_bindings()[0].fetch_constant;
    stream.stride_words = draw.stream_stride_dwords;
    stream.bytes = stream_bytes.data();
    stream.byte_count = uint32_t(stream_bytes.size());
    stream.indices = indices.data();
    stream.index_count = uint32_t(indices.size());
    stream.endian = endian;
    std::vector<uint8_t> pixels;
    if (!device.DrawGuestFragment(vertex_words.data(), vertex_words.size(), kWidth, kHeight,
                                  image_count, sampler_count, 1, false, 0.f, true, &stream, 1,
                                  pixels, reason, pixel_words.data(), pixel_words.size(),
                                  texture_rgba, texture_width, texture_height, raster, floats.data(),
                                  floats.data() + 1024)) {
      if (raster != nullptr) {
        std::cout << "mesh " << label << " failed: " << reason << "\n";
        return;
      }
      Fail(reason.empty() ? "mesh draw failed" : reason);
    }
    uint32_t covered = 0;
    int first[4] = {-1, -1, -1, -1};
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      if (pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 255 && pixels[i + 3] == 255) {
        continue;
      }
      if (first[0] < 0) {
        first[0] = pixels[i];
        first[1] = pixels[i + 1];
        first[2] = pixels[i + 2];
        first[3] = pixels[i + 3];
      }
      ++covered;
    }
    std::cout << "mesh " << label << " covered " << covered << " first " << first[0] << " "
              << first[1] << " " << first[2] << " " << first[3] << "\n";
    if (label[0] == 'e' && label[7] == '1' && texture_rgba != nullptr) {
      std::vector<uint8_t> plain;
      std::string plain_reason;
      if (device.DrawGuestFragment(vertex_words.data(), vertex_words.size(), kWidth, kHeight,
                                   image_count, sampler_count, 1, false, 0.f, true, &stream, 1,
                                   plain, plain_reason, pixel_words.data(), pixel_words.size(),
                                   nullptr, 0, 0, nullptr, floats.data(), floats.data() + 1024)) {
        uint32_t differ = 0;
        const size_t n = std::min(pixels.size(), plain.size());
        for (size_t i = 0; i < n; ++i) {
          if (pixels[i] != plain[i]) {
            ++differ;
          }
        }
        std::cout << "mesh texture differs " << differ << "\n";
      }
    }
  };
  draw_mesh(1u, nullptr, "endian 1");
  draw_mesh(2u, nullptr, "endian 2");
  broken_bond::gfx::Device::RasterState raster;
  raster.surface_info = draw.surface_info & ~0x30000u;
  raster.depth_control = draw.depth_control;
  raster.blend_control = draw.blend_control;
  raster.mode_cntl = draw.mode_cntl;
  raster.clear_red = 1.f;
  raster.clear_green = 0.f;
  raster.clear_blue = 1.f;
  raster.clear_alpha = 1.f;
  std::cout << "mesh surface msaa " << ((draw.surface_info >> 16) & 3u) << "\n";
  draw_mesh(1u, &raster, "raster");
  broken_bond::gfx::Device::RasterState depth_off = raster;
  depth_off.depth_control &= ~2u;
  draw_mesh(1u, &depth_off, "depth off");
  broken_bond::gfx::Device::RasterState cull_off = raster;
  cull_off.mode_cntl &= ~3u;
  draw_mesh(1u, &cull_off, "cull off");
  std::cout << "mesh_draw_ok 1\n";
  return 0;
}
#endif
#endif

std::string ReadName(const uint8_t* bytes) {
  size_t length = 0;
  while (length < 64 && bytes[length] != 0) {
    if (bytes[length] < 32 || bytes[length] > 126) {
      Fail("source name is not ascii");
    }
    ++length;
  }
  if (length == 0 || length == 64) {
    Fail("source name is empty or not terminated");
  }
  return std::string(reinterpret_cast<const char*>(bytes), length);
}

#if defined(SHADER_DB_TRANSLATE)
struct TileResult {
  uint32_t formats = 0;
  uint32_t checked = 0;
  uint32_t skipped = 0;
  uint32_t blocks = 0;
  uint32_t address_failed = 0;
  uint32_t duplicate = 0;
  uint32_t roundtrip_failed = 0;
  uint32_t volume_blocks = 0;
  uint32_t volume_failed = 0;
  uint32_t below_origin = 0;
  uint32_t below_origin_max = 0;
  uint32_t span_exceeded = 0;
  uint32_t lower_bound_mismatch = 0;
  uint32_t lower_bound_above = 0;
  uint32_t texel_lower_above = 0;
  bool endian_ok = false;
  std::string skipped_names;
  std::string example;
};

uint32_t BlockLog2(uint32_t bytes) {
  uint32_t log2 = 0;
  for (uint32_t n = bytes; n > 1; n >>= 1) {
    ++log2;
  }
  return log2;
}

bool CheckTileWindow(const rex::graphics::FormatInfo* format, uint32_t pitch, uint32_t offset_x,
                     uint32_t offset_y, uint32_t width, uint32_t height, TileResult& result) {
  const uint32_t bytes = format->bytes_per_block();
  const uint32_t log2 = BlockLog2(bytes);
  const uint32_t untile_log2 = (bytes / 4) + ((bytes / 2) >> (bytes / 4));
  if (bytes == 0 || (uint32_t(1) << log2) != bytes || untile_log2 != log2) {
    return false;
  }
  const uint32_t bound = rex::graphics::texture_util::GetTiledAddressUpperBound2D(
      offset_x + width, offset_y + height, pitch, log2);
  std::vector<uint8_t> tiled(static_cast<size_t>(bound) + bytes);
  std::vector<uint8_t> output(static_cast<size_t>(width) * height * bytes);
  std::vector<uint32_t> addresses;
  addresses.reserve(static_cast<size_t>(width) * height);
  rex::graphics::texture_conversion::UntileInfo info{};
  info.offset_x = offset_x;
  info.offset_y = offset_y;
  info.width = width;
  info.height = height;
  info.input_pitch = pitch;
  info.output_pitch = width;
  info.input_format_info = format;
  info.output_format_info = format;
  info.copy_callback = [&](void* output, const void* input, size_t length) {
    addresses.push_back(static_cast<uint32_t>(static_cast<const uint8_t*>(input) - tiled.data()));
    if (output != nullptr && length != 0) {
      std::memset(output, 0, length);
    }
  };
  rex::graphics::texture_conversion::Untile(output.data(), tiled.data(), &info);
  std::set<uint32_t> seen;
  bool ok = addresses.size() == static_cast<size_t>(width) * height;
  for (uint32_t y = 0; y < height && ok; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t got = addresses[static_cast<size_t>(y) * width + x];
      const uint32_t expected = static_cast<uint32_t>(rex::graphics::texture_util::GetTiledOffset2D(
          int32_t(offset_x + x), int32_t(offset_y + y), pitch, log2));
      if (!seen.insert(got).second) {
        ++result.duplicate;
        ok = false;
      }
      if (got != expected || got + bytes > tiled.size()) {
        ++result.address_failed;
        if (result.example.empty()) {
          result.example = std::string(format->name) + " pitch " + std::to_string(pitch) + " x " +
                           std::to_string(offset_x + x) + " y " + std::to_string(offset_y + y) +
                           " untile " + std::to_string(got) + " address " + std::to_string(expected);
        }
        ok = false;
        break;
      }
    }
  }
  result.blocks += static_cast<uint32_t>(addresses.size());
  ++result.checked;
  return ok;
}

void CheckTiling3D(TileResult& result);

TileResult CheckTiling() {
  TileResult result;
  uint8_t endian_in[4] = {0x11, 0x22, 0x33, 0x44};
  uint8_t endian_out[4] = {};
  auto swapped = [&](rex::graphics::xenos::Endian endian, uint8_t a, uint8_t b, uint8_t c,
                     uint8_t d) {
    rex::graphics::texture_conversion::CopySwapBlock(endian, endian_out, endian_in, 4);
    return endian_out[0] == a && endian_out[1] == b && endian_out[2] == c && endian_out[3] == d;
  };
  result.endian_ok = swapped(rex::graphics::xenos::Endian::kNone, 0x11, 0x22, 0x33, 0x44) &&
                     swapped(rex::graphics::xenos::Endian::k8in16, 0x22, 0x11, 0x44, 0x33) &&
                     swapped(rex::graphics::xenos::Endian::k8in32, 0x44, 0x33, 0x22, 0x11) &&
                     swapped(rex::graphics::xenos::Endian::k16in32, 0x33, 0x44, 0x11, 0x22);

  for (uint32_t format_index = 0; format_index < 64; ++format_index) {
    const rex::graphics::FormatInfo* format = rex::graphics::FormatInfo::Get(format_index);
    ++result.formats;
    const uint32_t bytes = format->bytes_per_block();
    const uint32_t log2 = BlockLog2(bytes);
    if (bytes == 0 || (uint32_t(1) << log2) != bytes) {
      ++result.skipped;
      if (!result.skipped_names.empty()) {
        result.skipped_names += " ";
      }
      result.skipped_names += format->name;
      continue;
    }
    const uint32_t pitches[] = {32, 64, 96, 128};
    for (uint32_t pitch : pitches) {
      CheckTileWindow(format, pitch, 0, 0, pitch, 64, result);
      CheckTileWindow(format, pitch, 8, 4, 24, 20, result);
    }
  }

  const rex::graphics::FormatInfo* rgba = rex::graphics::FormatInfo::Get(
      static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_8_8_8_8));
  constexpr uint32_t kPitch = 64;
  constexpr uint32_t kHeight = 48;
  const uint32_t bytes = rgba->bytes_per_block();
  const uint32_t log2 = BlockLog2(bytes);
  const uint32_t bound =
      rex::graphics::texture_util::GetTiledAddressUpperBound2D(kPitch, kHeight, kPitch, log2);
  std::vector<uint8_t> linear(static_cast<size_t>(kPitch) * kHeight * bytes);
  std::vector<uint8_t> tiled(static_cast<size_t>(bound) + bytes);
  std::vector<uint8_t> restored(linear.size());
  for (size_t i = 0; i < linear.size(); ++i) {
    linear[i] = static_cast<uint8_t>(i * 17u + 3u);
  }
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kPitch; ++x) {
      const uint32_t address = static_cast<uint32_t>(
          rex::graphics::texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), kPitch, log2));
      std::memcpy(tiled.data() + address, linear.data() + (static_cast<size_t>(y) * kPitch + x) * bytes,
                  bytes);
    }
  }
  rex::graphics::texture_conversion::UntileInfo info{};
  info.width = kPitch;
  info.height = kHeight;
  info.input_pitch = kPitch;
  info.output_pitch = kPitch;
  info.input_format_info = rgba;
  info.output_format_info = rgba;
  info.copy_callback = [](void* output, const void* input, size_t length) {
    std::memcpy(output, input, length);
  };
  rex::graphics::texture_conversion::Untile(restored.data(), tiled.data(), &info);
  result.roundtrip_failed = restored == linear ? 0 : 1;
  CheckTiling3D(result);
  return result;
}

uint32_t Address3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height,
                   uint32_t log2) {
  return static_cast<uint32_t>(rex::graphics::texture_util::GetTiledOffset3D(
      int32_t(x), int32_t(y), int32_t(z), pitch, height, log2));
}

uint32_t TileOriginInRow3D(uint32_t x, uint32_t log2) {
  if (log2 == 0) {
    return ((x >> 6) << 12) | (((x >> 5) & 1u) << 10);
  }
  return (x / 32u) * 0x1000u * ((1u << log2) / 2u);
}

uint32_t TileSpan3D(uint32_t pitch, uint32_t log2) {
  if (log2 == 0) {
    return ((pitch >> 6) << 12) + 0xC00u + ((pitch & 32u) << 5);
  }
  return ((pitch / 32u) * 0x1000u + 0x1000u) * ((1u << log2) / 2u);
}

void FailVolume(TileResult& result, const std::string& text) {
  ++result.volume_failed;
  if (result.example.empty()) {
    result.example = text;
  }
}

void CheckLinearRun3D(uint32_t pitch, uint32_t height, uint32_t log2, TileResult& result) {
  const uint32_t bytes = 1u << log2;
  const uint32_t run = log2 == 0 || log2 == 1 ? 8u : log2 == 2 ? 4u : log2 == 3 ? 2u : 1u;
  const uint32_t gap = log2 <= 1 ? 64u : 32u;
  const uint32_t base = Address3D(0, 0, 0, pitch, height, log2);
  if (base != 0) {
    FailVolume(result, "3d origin " + std::to_string(log2) + " is " + std::to_string(base));
  }
  for (uint32_t i = 1; i < run; ++i) {
    const uint32_t got = Address3D(i, 0, 0, pitch, height, log2);
    if (got != base + i * bytes) {
      FailVolume(result, "3d run log2 " + std::to_string(log2) + " x " + std::to_string(i));
      return;
    }
  }
  const uint32_t odd = Address3D(run, 0, 0, pitch, height, log2);
  if (odd != base + gap) {
    FailVolume(result, "3d gap log2 " + std::to_string(log2) + " got " + std::to_string(odd));
  }
}

void CheckVolume3D(uint32_t pitch, uint32_t height, uint32_t depth, uint32_t log2,
                   TileResult& result) {
  const uint32_t bytes = 1u << log2;
  const uint32_t span = TileSpan3D(pitch, log2);
  const uint32_t upper = rex::graphics::texture_util::GetTiledAddressUpperBound3D(
      pitch, height, depth, pitch, height, log2);
  const uint32_t step_z = Address3D(0, 0, 8, pitch, height, log2);
  struct TileExtent {
    uint32_t origin = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    uint32_t low = 0xffffffffu;
    uint32_t high = 0;
  };
  std::map<uint32_t, TileExtent> tiles;
  std::vector<uint32_t> seen;
  seen.reserve(static_cast<size_t>(pitch) * height * depth);
  for (uint32_t z = 0; z < depth; ++z) {
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < pitch; ++x) {
        const uint32_t address = Address3D(x, y, z, pitch, height, log2);
        const uint32_t origin = Address3D(x & ~31u, y & ~31u, z & ~3u, pitch, height, log2);
        seen.push_back(address);
        ++result.volume_blocks;
        if (address < origin) {
          ++result.below_origin;
          result.below_origin_max = std::max(result.below_origin_max, origin - address);
        }
        const uint32_t sdk_texel = rex::graphics::texture_util::GetTiledAddressLowerBound3D(
            x, y, z, pitch, height, log2);
        if (sdk_texel > address) {
          ++result.texel_lower_above;
        }
        if (address + bytes > upper) {
          FailVolume(result, "3d upper log2 " + std::to_string(log2) + " pitch " +
                                 std::to_string(pitch) + " x " + std::to_string(x) + " y " +
                                 std::to_string(y) + " z " + std::to_string(z) + " address " +
                                 std::to_string(address) + " upper " + std::to_string(upper));
          return;
        }
        const uint32_t key = (x >> 5) | ((y >> 5) << 6) | ((z >> 2) << 12);
        TileExtent& tile = tiles[key];
        tile.origin = origin;
        tile.x = x & ~31u;
        tile.y = y & ~31u;
        tile.z = z & ~3u;
        tile.low = std::min(tile.low, address);
        tile.high = std::max(tile.high, address);
        if ((x & 31u) == 0 && (y & 31u) == 0 && (z & 3u) == 0) {
          const uint32_t row = Address3D(0, y, z, pitch, height, log2);
          if (address - row != TileOriginInRow3D(x, log2) ||
              (x >= 32 && address <= Address3D(x - 32, y, z, pitch, height, log2))) {
            FailVolume(result, "3d origin log2 " + std::to_string(log2) + " pitch " +
                                   std::to_string(pitch) + " x " + std::to_string(x) + " z " +
                                   std::to_string(z));
            return;
          }
        }
        if (z < 8) {
          const uint32_t ahead = Address3D(x, y, z + 8, pitch, height, log2);
          if (ahead - address != step_z) {
            FailVolume(result, "3d z-period log2 " + std::to_string(log2) + " x " +
                                   std::to_string(x) + " y " + std::to_string(y) + " z " +
                                   std::to_string(z));
            return;
          }
        }
        if (y < 32 && height >= 64) {
          const uint32_t step_y =
              Address3D(0, 32, z & ~3u, pitch, height, log2) - Address3D(0, 0, z & ~3u, pitch, height, log2);
          if (Address3D(x, y + 32, z, pitch, height, log2) - address != step_y) {
            FailVolume(result, "3d y-period log2 " + std::to_string(log2) + " pitch " +
                                   std::to_string(pitch) + " x " + std::to_string(x) + " z " +
                                   std::to_string(z));
            return;
          }
        }
      }
    }
  }
  std::sort(seen.begin(), seen.end());
  if (std::adjacent_find(seen.begin(), seen.end()) != seen.end()) {
    FailVolume(result, "3d duplicate log2 " + std::to_string(log2) + " pitch " +
                           std::to_string(pitch) + " height " + std::to_string(height));
  }
  for (const auto& entry : tiles) {
    const TileExtent& tile = entry.second;
    if (tile.high + bytes > tile.origin + span) {
      ++result.span_exceeded;
      if (result.example.empty()) {
        result.example = "3d span log2 " + std::to_string(log2) + " pitch " + std::to_string(pitch) +
                         " tile " + std::to_string(tile.x) + " " + std::to_string(tile.y) + " " +
                         std::to_string(tile.z) + " high " + std::to_string(tile.high) +
                         " origin " + std::to_string(tile.origin) + " span " + std::to_string(span);
      }
    }
    const uint32_t sdk = rex::graphics::texture_util::GetTiledAddressLowerBound3D(
        tile.x, tile.y, tile.z, pitch, height, log2);
    if (sdk != tile.origin) {
      ++result.lower_bound_mismatch;
    }
    if (sdk > tile.low) {
      ++result.lower_bound_above;
    }
  }
}

void CheckTiling3D(TileResult& result) {
  const uint32_t pitches[] = {32, 64, 96, 128};
  const uint32_t heights[] = {32, 64};
  for (uint32_t log2 = 0; log2 <= 4; ++log2) {
    for (uint32_t pitch : pitches) {
      CheckLinearRun3D(pitch, 32, log2, result);
      for (uint32_t height : heights) {
        CheckVolume3D(pitch, height, 16, log2, result);
        if (result.volume_failed != 0) {
          return;
        }
      }
    }
  }
}
#endif

}  // namespace

int main(int argc, char** argv) {
  try {
#if defined(SHADER_DB_TRANSLATE) && defined(BROKEN_BOND_HOST_GFX)
    if (argc == 4 && std::string(argv[2]) == "mesh") {
      return DrawMesh(argv[3]);
    }
#endif
    int tiling_failed = 0;
    int guest_draw_failed = 0;
    int semantic_failed = 0;
#if defined(SHADER_DB_TRANSLATE)
    const TileResult tiling = CheckTiling();
    tiling_failed = (tiling.address_failed != 0 || tiling.duplicate != 0 ||
                     tiling.roundtrip_failed != 0 || tiling.volume_failed != 0 || !tiling.endian_ok)
                        ? 1
                        : 0;
    std::cout << "tiling\n";
    std::cout << "  formats " << tiling.formats << "\n";
    std::cout << "  checked " << tiling.checked << "\n";
    std::cout << "  skipped " << tiling.skipped << " " << tiling.skipped_names << "\n";
    std::cout << "  blocks " << tiling.blocks << "\n";
    std::cout << "  address_failed " << tiling.address_failed << "\n";
    std::cout << "  duplicate " << tiling.duplicate << "\n";
    std::cout << "  roundtrip_failed " << tiling.roundtrip_failed << "\n";
    std::cout << "  volume_blocks " << tiling.volume_blocks << "\n";
    std::cout << "  volume_failed " << tiling.volume_failed << "\n";
    std::cout << "  below_origin " << tiling.below_origin << "\n";
    std::cout << "  below_origin_max " << tiling.below_origin_max << "\n";
    std::cout << "  span_exceeded " << tiling.span_exceeded << "\n";
    std::cout << "  lower_bound_mismatch " << tiling.lower_bound_mismatch << "\n";
    std::cout << "  lower_bound_above " << tiling.lower_bound_above << "\n";
    std::cout << "  texel_lower_above " << tiling.texel_lower_above << "\n";
    std::cout << "  endian_ok " << (tiling.endian_ok ? 1 : 0) << "\n";
    if (!tiling.example.empty()) {
      std::cout << "  example " << tiling.example << "\n";
    }
    std::cout.flush();
#endif
    const bool boot_pair = argc == 3 && std::string(argv[2]) == "pair";
    if (argc != 2 && !boot_pair) {
      std::cerr << "usage: shader_db <xeshader.sdb> [pair]\n";
      return 2;
    }
    const char* path = argv[1];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      Fail(std::string("cannot open ") + path);
    }
    std::vector<uint8_t> file(std::istreambuf_iterator<char>(in), {});
    if (file.size() < 24 || ReadBe32(file.data()) != 0x33424453u) {
      Fail("not a 3BDS shader database");
    }
    const uint32_t version = ReadBe32(file.data() + 4);
    const uint32_t source_count = ReadBe32(file.data() + 8);
    const uint32_t shader_count = ReadBe32(file.data() + 12);
    const uint32_t source_offset = ReadBe32(file.data() + 16);
    const uint32_t shader_offset = ReadBe32(file.data() + 20);
    if (version != 0x00040001u || source_offset != 24) {
      Fail("unsupported shader database version");
    }

    std::vector<Source> sources;
    size_t cursor = source_offset;
    for (uint32_t i = 0; i < source_count; ++i) {
      if (cursor + 80 > file.size()) {
        Fail("source entry runs past the file");
      }
      Source source;
      source.name = ReadName(file.data() + cursor);
      const uint32_t compressed = ReadBe32(file.data() + cursor + 68);
      source.raw_size = ReadBe32(file.data() + cursor + 72);
      const uint32_t index = ReadBe32(file.data() + cursor + 76);
      if (index != i || cursor + 80 + compressed > file.size()) {
        Fail("source entry is truncated");
      }
      InflateZlib(file.data() + cursor + 80, compressed, source.raw_size);
      sources.push_back(std::move(source));
      cursor += 80 + compressed;
    }
    if (cursor != shader_offset) {
      Fail("source section does not end at the shader table");
    }

    constexpr size_t kPreamble = 32;
    constexpr size_t kRecord = 44;
    if (shader_offset + kPreamble + shader_count * kRecord > file.size()) {
      Fail("shader table runs past the file");
    }
    std::vector<ShaderRecord> shaders;
    shaders.reserve(shader_count);
    size_t record = shader_offset + kPreamble;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < shader_count; ++i) {
      ShaderRecord shader;
      shader.offset = ReadBe32(file.data() + record);
      shader.compressed_size = ReadBe32(file.data() + record + 4);
      shader.raw_size = ReadBe32(file.data() + record + 8);
      shader.tag = ReadBe32(file.data() + record + 12);
      if (shader.offset < previous || shader.compressed_size < 6 ||
          shader.offset + shader.compressed_size > file.size()) {
        Fail("shader payload is outside the file");
      }
      previous = shader.offset;
      shaders.push_back(shader);
      record += kRecord;
    }

#if defined(SHADER_DB_TRANSLATE) && defined(BROKEN_BOND_HOST_GFX)
    if (boot_pair) {
      return DrawBootPair(file, shaders);
    }
#endif

    uint32_t vertex = 0;
    uint32_t pixel = 0;
#if defined(BROKEN_BOND_HOST_GFX)
    uint32_t semantic_ok = 0;
    std::map<uint32_t, uint32_t> semantic_uses;
    std::string semantic_reason;
#endif
    uint32_t other = 0;
    uint64_t raw_bytes = 0;
    uint64_t ucode_bytes = 0;
    uint32_t tag_count[3] = {};
    ConstantStats constants;
#if defined(SHADER_DB_TRANSLATE)
    HostProbe probe;
    probe.Open();
    rex::graphics::SpirvShaderTranslator::Features features(false);
    if (probe.create_module != nullptr && probe.api >= VK_API_VERSION_1_2) {
      features.spirv_version = spv::Spv_1_5;
      features.max_storage_buffer_range = probe.storage_range;
      features.signed_zero_inf_nan_preserve_float32 = probe.preserve;
      features.denorm_flush_to_zero_float32 = probe.flush;
      features.rounding_mode_rte_float32 = probe.rte;
      features.demote_to_helper_invocation = probe.demote;
      features.sample_rate_shading = probe.sample_rate;
    } else {
      features = rex::graphics::SpirvShaderTranslator::Features(true);
    }
    rex::graphics::SpirvShaderTranslator translator(features, false, false, false);
    TranslateStats translated;
#endif
    for (const ShaderRecord& shader : shaders) {
      const std::vector<uint8_t> object =
          InflateZlib(file.data() + shader.offset, shader.compressed_size, shader.raw_size);
      raw_bytes += object.size();
      if (object.size() < 52) {
        Fail("shader object is shorter than its version token");
      }
      const uint32_t token = ReadBe32(object.data() + 48);
#if defined(BROKEN_BOND_HOST_GFX)
      if (token == 0xFFFE0300u) {
        std::vector<broken_bond::gfx::VertexFetchSemantic> semantics;
        std::string reason;
        if (!broken_bond::gfx::ReadVertexFetchSemantics(object.data(), object.size(), semantics,
                                                       reason)) {
          semantic_failed = 1;
          if (semantic_reason.empty()) {
            semantic_reason = reason;
          }
        } else {
          ++semantic_ok;
          for (const broken_bond::gfx::VertexFetchSemantic& semantic : semantics) {
            ++semantic_uses[(semantic.usage << 8) | semantic.usage_index];
          }
        }
      }
#endif
      const UcodeSpan ucode = LocateUcode(object.data(), object.size(), token);
      ucode_bytes += uint64_t(ucode.dword_count) * 4;
      ReadConstants(object.data(), object.size(), token, constants);
#if defined(SHADER_DB_TRANSLATE)
      TranslateShader(translator, token, ucode, translated, probe, object.data(), object.size());
#endif
      if (token == 0xFFFE0300u) {
        ++vertex;
      } else if (token == 0xFFFF0300u) {
        ++pixel;
      } else {
        ++other;
      }
      if (shader.tag < 3) {
        ++tag_count[shader.tag];
      }
    }

    std::cout << "shader db version 4.1\n";
    std::cout << "sources " << sources.size() << "\n";
    for (const Source& source : sources) {
      std::cout << "  " << source.name << " " << source.raw_size << "\n";
    }
    std::cout << "shaders " << shaders.size() << "\n";
    std::cout << "  vs_3_0 " << vertex << "\n";
    std::cout << "  ps_3_0 " << pixel << "\n";
    std::cout << "  other " << other << "\n";
    std::cout << "  tag0 " << tag_count[0] << " tag1 " << tag_count[1] << " tag2 " << tag_count[2]
              << "\n";
    std::cout << "inflated " << raw_bytes << "\n";
    std::cout << "ucode " << ucode_bytes << "\n";
    std::cout << "constants " << (constants.float4 + constants.sampler) << "\n";
    std::cout << "  float4 " << constants.float4 << "\n";
    std::cout << "  sampler " << constants.sampler << "\n";
    std::cout << "  names " << constants.names.size() << "\n";
#if defined(SHADER_DB_TRANSLATE)
    std::cout << "spirv " << (translated.ok + translated.failed) << "\n";
    std::cout << "  host preserve " << (features.signed_zero_inf_nan_preserve_float32 ? 1 : 0)
              << " flush " << (features.denorm_flush_to_zero_float32 ? 1 : 0) << " rte "
              << (features.rounding_mode_rte_float32 ? 1 : 0) << " sample_rate "
              << (features.sample_rate_shading ? 1 : 0) << " demote "
              << (features.demote_to_helper_invocation ? 1 : 0) << "\n";
    std::cout << "  ok " << translated.ok << "\n";
    std::cout << "  failed " << translated.failed << "\n";
    std::cout << "  bytes " << translated.spirv_bytes << "\n";
    std::cout << "  dynamic_registers " << translated.dynamic_registers << "\n";
    std::cout << "  memexport " << translated.memexport << "\n";
    std::cout << "  depth " << translated.depth << "\n";
    std::cout << "  textures " << translated.texture_shaders << " max " << translated.texture_max
              << "\n";
    std::cout << "  samplers " << translated.sampler_shaders << " max " << translated.sampler_max
              << "\n";
    std::cout << "  vertex_textures " << translated.vertex_texture_shaders << " max "
              << translated.vertex_texture_max << " samplers " << translated.vertex_sampler_max
              << "\n";
    std::cout << "  pixel_textures max " << translated.pixel_texture_max << " samplers "
              << translated.pixel_sampler_max << "\n";
    std::cout << "  float_constants " << translated.float_shaders << " max "
              << translated.float_max << "\n";
    for (const auto& version : translated.versions) {
      const uint32_t major = (version.first >> 16) & 0xFFu;
      const uint32_t minor = (version.first >> 8) & 0xFFu;
      std::cout << "  version " << major << "." << minor << " " << version.second << "\n";
    }
    for (const auto& capability : translated.capabilities) {
      const char* name = CapabilityName(capability.first);
      std::cout << "  cap " << (name != nullptr ? name : std::to_string(capability.first)) << " "
                << capability.second << "\n";
    }
    for (const auto& extension : translated.extensions) {
      std::cout << "  ext " << extension.first << " " << extension.second << "\n";
    }
    for (const auto& descriptor : translated.descriptors) {
      std::cout << "  set " << (descriptor.first >> 32) << " binding "
                << uint32_t(descriptor.first) << " " << descriptor.second << "\n";
    }
#if defined(SHADER_DB_VULKAN_SDK)
    if (probe.context == nullptr) {
      std::cout << "validator closed " << probe.validator_reason << "\n";
    } else {
      std::cout << "validator vulkan-1.2 scalar-block\n";
      std::cout << "  ok " << probe.val_ok << "\n";
      std::cout << "  failed " << probe.val_failed << "\n";
      std::vector<std::pair<uint32_t, std::string>> validation;
      validation.reserve(probe.val_messages.size());
      for (const auto& entry : probe.val_messages) {
        validation.emplace_back(entry.second, entry.first);
      }
      std::sort(validation.begin(), validation.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      const size_t validation_shown = std::min<size_t>(validation.size(), 8);
      for (size_t i = 0; i < validation_shown; ++i) {
        std::cout << "  message " << validation[i].first << " " << validation[i].second << "\n";
      }
    }
#endif
    if (probe.create_module == nullptr) {
      std::cout << "vulkan closed " << probe.vulkan_reason << "\n";
    } else {
      std::cout << "vulkan " << probe.device_name << "\n";
      std::cout << "  api " << VK_API_VERSION_MAJOR(probe.api) << "."
                << VK_API_VERSION_MINOR(probe.api) << "." << VK_API_VERSION_PATCH(probe.api)
                << "\n";
      if (!probe.driver_name.empty()) {
        std::cout << "  driver " << probe.driver_name << "\n";
      }
      std::cout << "  storage_range " << probe.storage_range << "\n";
      std::cout << "  float32_preserve " << (probe.preserve ? 1 : 0) << "\n";
      std::cout << "  float32_flush " << (probe.flush ? 1 : 0) << "\n";
      std::cout << "  float32_rte " << (probe.rte ? 1 : 0) << "\n";
      std::cout << "  sample_rate " << (probe.sample_rate ? 1 : 0) << "\n";
      std::cout << "  demote " << (probe.demote ? 1 : 0) << "\n";
      std::cout << "  scalar_block " << (probe.scalar_block ? 1 : 0) << "\n";
      std::cout << "  standard_layout " << (probe.standard_layout ? 1 : 0) << "\n";
      std::cout << "  modules " << (probe.module_ok + probe.module_failed) << "\n";
      std::cout << "  ok " << probe.module_ok << "\n";
      std::cout << "  failed " << probe.module_failed << "\n";
      std::cout << "  vertex_pipelines " << (probe.pipeline_ok + probe.pipeline_failed) << "\n";
      std::cout << "  vertex_pipeline_ok " << probe.pipeline_ok << "\n";
      std::cout << "  vertex_pipeline_failed " << probe.pipeline_failed << "\n";
      for (const auto& pipeline_result : probe.pipeline_results) {
        std::cout << "  vertex_pipeline_result " << pipeline_result.first << " "
                  << pipeline_result.second << "\n";
      }
      std::cout << "  pixel_pipelines " << (probe.pixel_ok + probe.pixel_failed) << "\n";
      std::cout << "  pixel_pipeline_ok " << probe.pixel_ok << "\n";
      std::cout << "  pixel_pipeline_failed " << probe.pixel_failed << "\n";
      std::cout << "  pixel_layouts " << probe.pixel_layouts.size() << "\n";
      for (const auto& pipeline_result : probe.pixel_results) {
        std::cout << "  pixel_pipeline_result " << pipeline_result.first << " "
                  << pipeline_result.second << "\n";
      }
      for (const auto& result : probe.module_results) {
        std::cout << "  result " << result.first << " " << result.second << "\n";
      }
    }
    std::vector<std::pair<uint32_t, std::string>> ranked;
    ranked.reserve(translated.fatals.size());
    for (const auto& entry : translated.fatals) {
      ranked.emplace_back(entry.second, entry.first);
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    const size_t shown = std::min<size_t>(ranked.size(), 8);
    for (size_t i = 0; i < shown; ++i) {
      std::cout << "  fatal " << ranked[i].first << " " << ranked[i].second << "\n";
    }
#if defined(BROKEN_BOND_HOST_GFX)
    std::string guest_reason = "host device did not open";
    broken_bond::gfx::Device device;
    const bool guest_open = device.Open(guest_reason);
    uint32_t guest_draws = 0;
    uint32_t pack_seen = 0;
    uint32_t pack_bad = 0;
    uint32_t slot_bad = 0;
    bool fetch_sample_done = false;
    constexpr uint32_t kGuestSize = 8;
    std::vector<uint32_t> link_fragment;
    const auto draw_batch = [&](const std::vector<TranslateStats::GuestDraw>& batch,
                                const char* kind) {
      for (const TranslateStats::GuestDraw& fragment : batch) {
        if (!guest_open) {
          std::cout << kind << " failed " << guest_reason << "\n";
          continue;
        }
        std::vector<uint8_t> drawn;
        std::vector<uint8_t> other;
        const bool first_ok =
            fragment.vertex
                ? device.DrawGuestFragment(fragment.words.data(), fragment.words.size(), kGuestSize,
                                           kGuestSize, 0, 0, 0, false, 0.f, true,
                                           fragment.streams.data(), uint32_t(fragment.streams.size()),
                                           drawn, guest_reason)
                : device.DrawGuestFragment(fragment.words.data(), fragment.words.size(), kGuestSize,
                                           kGuestSize, fragment.images, fragment.samplers, 0,
                                           fragment.interpolate, 1.f, false, nullptr, 0, drawn,
                                           guest_reason);
        if (!first_ok) {
          std::cout << kind << " failed images " << fragment.images << " samplers "
                    << fragment.samplers << " " << guest_reason << "\n";
          continue;
        }
        uint32_t changed = 0;
        uint32_t texture_diff = 0;
        uint32_t other_changed = 0;
        bool second = false;
        if (fragment.declare_test && !fragment.streams.empty()) {
          std::vector<broken_bond::gfx::VertexStream> swapped = fragment.streams;
          swapped[0].triangle_attribute = 1;
          second = device.DrawGuestFragment(fragment.words.data(), fragment.words.size(), kGuestSize,
                                            kGuestSize, 0, 0, 0, false, 0.f, true, swapped.data(),
                                            uint32_t(swapped.size()), other, guest_reason);
        } else if (fragment.vertex && !fragment.streams.empty() &&
                   fragment.streams[0].attribute_count > 0 &&
                   fragment.streams[0].attributes[0].short4) {
          std::vector<broken_bond::gfx::VertexStream> scaled = fragment.streams;
          scaled[0].short_scale = 4;
          scaled[0].short_w_scale = 2;
          second = device.DrawGuestFragment(fragment.words.data(), fragment.words.size(), kGuestSize,
                                            kGuestSize, 0, 0, 0, false, 0.f, true, scaled.data(),
                                            uint32_t(scaled.size()), other, guest_reason);
        } else if (!fragment.vertex && fragment.interpolate) {
          second = device.DrawGuestFragment(fragment.words.data(), fragment.words.size(), kGuestSize,
                                            kGuestSize, 0, 0, 0, true, 0.f, false, nullptr, 0, other,
                                            guest_reason);
        } else if (!fragment.vertex && fragment.images != 0) {
          second = device.DrawGuestFragment(fragment.words.data(), fragment.words.size(), kGuestSize,
                                            kGuestSize, fragment.images, fragment.samplers, 1, false,
                                            0.f, false, nullptr, 0, other, guest_reason);
        }
        for (size_t i = 0; i + 3 < drawn.size(); i += 4) {
          if (drawn[i] != 255 || drawn[i + 1] != 0 || drawn[i + 2] != 255 || drawn[i + 3] != 255) {
            ++changed;
          }
          if (second && i + 3 < other.size() &&
              (drawn[i] != other[i] || drawn[i + 1] != other[i + 1] || drawn[i + 2] != other[i + 2] ||
               drawn[i + 3] != other[i + 3])) {
            ++texture_diff;
          }
          if (second && i + 3 < other.size() &&
              (other[i] != 255 || other[i + 1] != 0 || other[i + 2] != 255 || other[i + 3] != 255)) {
            ++other_changed;
          }
        }
        std::cout << kind << " ok images " << fragment.images << " samplers " << fragment.samplers
                  << " streams " << fragment.streams.size();
        if (!fragment.streams.empty()) {
          const broken_bond::gfx::VertexStream& stream = fragment.streams[0];
          uint32_t floats = 0;
          for (uint32_t a = 0; a < stream.attribute_count && a < 8; ++a) {
            if (stream.attributes[a].is_float) {
              ++floats;
            }
          }
          std::cout << " fetch " << stream.fetch_constant << " stride " << stream.stride_words
                    << " attrs " << stream.attribute_count << " floats " << floats << " index "
                    << stream.index_register;
          if (stream.attribute_count != 0) {
            std::cout << " offset0 " << stream.attributes[0].offset_words << " comps0 "
                      << stream.attributes[0].components;
          }
        }
        std::cout << " changed " << changed << " diff " << texture_diff;
        if (fragment.declare_test) {
          std::cout << " other_changed " << other_changed;
        }
        if (!fetch_sample_done && !fragment.vertex && fragment.images != 0 && second &&
            texture_diff > 0) {
          fetch_sample_done = true;
          constexpr uint32_t kTex = 32;
          std::vector<uint8_t> solid(static_cast<size_t>(kTex) * kTex * 4u, 0);
          for (size_t i = 0; i < solid.size(); i += 4) {
            solid[i] = 200;
            solid[i + 1] = 10;
            solid[i + 2] = 20;
            solid[i + 3] = 255;
          }
          uint32_t fetch_words[6] = {};
          std::vector<uint8_t> guest_tex;
          std::vector<uint8_t> restored;
          std::vector<uint8_t> sampled;
          const bool prepared =
              broken_bond::gfx::WriteRgba8Fetch(fetch_words, kTex, kTex, kTex, true, 2,
                                                guest_reason) &&
              broken_bond::gfx::StoreFetchRgba8(fetch_words, solid.data(), guest_tex, guest_reason) &&
              broken_bond::gfx::LoadFetchRgba8(fetch_words, guest_tex.data(), guest_tex.size(),
                                               restored, guest_reason) &&
              restored == solid;
          const bool sampled_ok =
              prepared &&
              device.DrawGuestFragment(fragment.words.data(), fragment.words.size(), kGuestSize,
                                       kGuestSize, fragment.images, fragment.samplers, 0, false, 0.f,
                                       false, nullptr, 0, sampled, guest_reason, nullptr, 0,
                                       restored.data(), kTex, kTex);
          uint32_t fetch_mismatch = 0;
          if (sampled_ok) {
            for (size_t i = 0; i + 3 < sampled.size() && i + 3 < other.size(); i += 4) {
              if (sampled[i] != other[i] || sampled[i + 1] != other[i + 1] ||
                  sampled[i + 2] != other[i + 2] || sampled[i + 3] != other[i + 3]) {
                ++fetch_mismatch;
              }
            }
          }
          std::cout << " fetch_sample " << (sampled_ok && fetch_mismatch == 0 ? 1 : 0);
          if (!sampled_ok) {
            std::cout << " fetch_sample_failed " << guest_reason;
          } else if (fetch_mismatch != 0) {
            std::cout << " fetch_mismatch " << fetch_mismatch;
          }
        }
        if (std::string(kind) == "guest_pack") {
          ++pack_seen;
          if (!second || changed != kGuestSize * kGuestSize || other_changed != 0) {
            ++pack_bad;
          }
          std::vector<broken_bond::gfx::VertexStream> filled = fragment.streams;
          for (broken_bond::gfx::VertexAttribute& attribute : filled[0].attributes) {
            if (attribute.is_float) {
              attribute.constant_fill = true;
            }
          }
          std::vector<uint8_t> filled_pixels;
          const bool fill_ok = device.DrawGuestFragment(
              fragment.words.data(), fragment.words.size(), kGuestSize, kGuestSize, 0, 0, 0, false,
              0.f, true, filled.data(), uint32_t(filled.size()), filled_pixels, guest_reason);
          uint32_t fill_diff = 0;
          if (fill_ok) {
            for (size_t i = 0; i + 3 < drawn.size() && i + 3 < filled_pixels.size(); i += 4) {
              if (drawn[i] != filled_pixels[i] || drawn[i + 1] != filled_pixels[i + 1] ||
                  drawn[i + 2] != filled_pixels[i + 2] || drawn[i + 3] != filled_pixels[i + 3]) {
                ++fill_diff;
              }
            }
          }
          std::cout << " interp " << fragment.interpolators << " fill_diff " << fill_diff;
          if (!fill_ok) {
            std::cout << " fill_failed " << guest_reason;
          }
          const uint32_t mask = fragment.interpolators & 0xFFFFu;
          struct Slot {
            uint32_t interpolator;
            uint32_t location;
          };
          std::vector<Slot> slots;
          uint32_t dense = 0;
          for (uint32_t bit = 0; bit < 16; ++bit) {
            if ((mask & (1u << bit)) == 0) {
              continue;
            }
            slots.push_back(Slot{bit, dense});
            ++dense;
          }
          auto count_diff = [](const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
            uint32_t diff = 0;
            for (size_t i = 0; i + 3 < left.size() && i + 3 < right.size(); i += 4) {
              if (left[i] != right[i] || left[i + 1] != right[i + 1] || left[i + 2] != right[i + 2] ||
                  left[i + 3] != right[i + 3]) {
                ++diff;
              }
            }
            return diff;
          };
          if (slots.empty()) {
            ++slot_bad;
            std::cout << " slots empty";
          } else {
            std::vector<std::vector<uint8_t>> baseline(slots.size());
            bool slots_ok = true;
            for (size_t s = 0; s < slots.size(); ++s) {
              std::vector<uint32_t> pass;
              if (!broken_bond::gfx::InterpolatorPassFragment(slots[s].location, pass) ||
                  !device.DrawGuestFragment(
                      fragment.words.data(), fragment.words.size(), kGuestSize, kGuestSize, 0, 0, 0,
                      false, 0.f, true, fragment.streams.data(), uint32_t(fragment.streams.size()),
                      baseline[s], guest_reason, pass.data(), pass.size())) {
                slots_ok = false;
                break;
              }
            }
            if (!slots_ok || !fill_ok) {
              ++slot_bad;
              if (!slots_ok) {
                std::cout << " slots_failed " << guest_reason;
              }
            } else {
              std::cout << " slots";
              std::vector<uint32_t> all_diff(slots.size(), 0);
              for (size_t s = 0; s < slots.size(); ++s) {
                std::vector<uint32_t> pass;
                std::vector<uint8_t> filled_slot;
                broken_bond::gfx::InterpolatorPassFragment(slots[s].location, pass);
                const bool ok = device.DrawGuestFragment(
                    fragment.words.data(), fragment.words.size(), kGuestSize, kGuestSize, 0, 0, 0,
                    false, 0.f, true, filled.data(), uint32_t(filled.size()), filled_slot,
                    guest_reason, pass.data(), pass.size());
                std::cout << " i" << slots[s].interpolator;
                if (!ok || filled_slot.size() < 4 || baseline[s].size() < 4) {
                  ++slot_bad;
                  std::cout << " fail";
                  continue;
                }
                all_diff[s] = count_diff(baseline[s], filled_slot);
                std::cout << " " << all_diff[s] << " " << int(baseline[s][0]) << ","
                          << int(baseline[s][1]) << "," << int(baseline[s][2]) << ","
                          << int(baseline[s][3]) << ">" << int(filled_slot[0]) << ","
                          << int(filled_slot[1]) << "," << int(filled_slot[2]) << ","
                          << int(filled_slot[3]);
              }
              std::vector<uint32_t> float_attributes;
              std::vector<std::vector<uint32_t>> attribute_diff;
              for (uint32_t a = 0; a < filled[0].attribute_count && a < 8; ++a) {
                if (!filled[0].attributes[a].is_float) {
                  continue;
                }
                float_attributes.push_back(a);
                attribute_diff.emplace_back(slots.size(), 0);
                std::vector<broken_bond::gfx::VertexStream> one = fragment.streams;
                one[0].attributes[a].constant_fill = true;
                std::cout << " " << broken_bond::gfx::VertexUsageName(one[0].attributes[a].usage)
                          << one[0].attributes[a].usage_index;
                for (size_t s = 0; s < slots.size(); ++s) {
                  std::vector<uint32_t> pass;
                  std::vector<uint8_t> one_pixels;
                  broken_bond::gfx::InterpolatorPassFragment(slots[s].location, pass);
                  const bool ok = device.DrawGuestFragment(
                      fragment.words.data(), fragment.words.size(), kGuestSize, kGuestSize, 0, 0, 0,
                      false, 0.f, true, one.data(), uint32_t(one.size()), one_pixels, guest_reason,
                      pass.data(), pass.size());
                  std::cout << " i" << slots[s].interpolator;
                  if (!ok) {
                    ++slot_bad;
                    std::cout << " fail";
                    continue;
                  }
                  attribute_diff.back()[s] = count_diff(baseline[s], one_pixels);
                  std::cout << " " << attribute_diff.back()[s];
                }
              }
              for (size_t s = 0; s < slots.size(); ++s) {
                if (all_diff[s] == 0) {
                  continue;
                }
                bool alone = false;
                for (const std::vector<uint32_t>& diff : attribute_diff) {
                  if (diff[s] != 0) {
                    alone = true;
                  }
                }
                if (alone || float_attributes.size() < 2) {
                  continue;
                }
                std::cout << " pair i" << slots[s].interpolator;
                for (size_t left = 0; left < float_attributes.size(); ++left) {
                  for (size_t right = left + 1; right < float_attributes.size(); ++right) {
                    std::vector<broken_bond::gfx::VertexStream> two = fragment.streams;
                    two[0].attributes[float_attributes[left]].constant_fill = true;
                    two[0].attributes[float_attributes[right]].constant_fill = true;
                    std::vector<uint32_t> pass;
                    std::vector<uint8_t> pair_pixels;
                    broken_bond::gfx::InterpolatorPassFragment(slots[s].location, pass);
                    const bool ok = device.DrawGuestFragment(
                        fragment.words.data(), fragment.words.size(), kGuestSize, kGuestSize, 0, 0,
                        0, false, 0.f, true, two.data(), uint32_t(two.size()), pair_pixels,
                        guest_reason, pass.data(), pass.size());
                    const broken_bond::gfx::VertexAttribute& first =
                        two[0].attributes[float_attributes[left]];
                    const broken_bond::gfx::VertexAttribute& second_attribute =
                        two[0].attributes[float_attributes[right]];
                    std::cout << " " << broken_bond::gfx::VertexUsageName(first.usage)
                              << first.usage_index << "+"
                              << broken_bond::gfx::VertexUsageName(second_attribute.usage)
                              << second_attribute.usage_index;
                    if (!ok || pair_pixels.size() < 4) {
                      ++slot_bad;
                      std::cout << " fail";
                      continue;
                    }
                    std::cout << " " << count_diff(baseline[s], pair_pixels) << " "
                              << int(pair_pixels[0]) << "," << int(pair_pixels[1]) << ","
                              << int(pair_pixels[2]) << "," << int(pair_pixels[3]);
                  }
                }
              }
            }
          }
        }
        if (drawn.size() >= 4) {
          std::cout << " first " << int(drawn[0]) << " " << int(drawn[1]) << " " << int(drawn[2])
                    << " " << int(drawn[3]);
        }
        if (second && other.size() >= 4) {
          std::cout << " second " << int(other[0]) << " " << int(other[1]) << " " << int(other[2])
                    << " " << int(other[3]);
        }
        if (!fragment.vertex && fragment.interpolate && second && drawn.size() >= 4 &&
            other.size() >= 4 && drawn[0] == 255 && drawn[1] == 64 && drawn[2] == 127 &&
            drawn[3] == 255 && other[0] == 0 && other[1] == 64 && other[2] == 127 &&
            other[3] == 255 && link_fragment.empty()) {
          link_fragment = fragment.words;
        }
        std::cout << "\n";
        if (changed != 0) {
          ++guest_draws;
        }
      }
    };
    std::cout << "guest_textured_seen " << translated.guest_textured_seen << "\n";
    std::cout << "guest_interpolated_seen " << translated.guest_interpolated_seen << "\n";
    std::cout << "guest_vertex_seen " << translated.guest_vertex_seen << "\n";
    std::cout << "guest_single_fetch " << translated.guest_fetch_seen << "\n";
    std::cout << "vertex_semantics " << semantic_ok << " failed " << semantic_failed;
    if (!semantic_reason.empty()) {
      std::cout << " reason " << semantic_reason;
    }
    std::cout << "\n";
    for (const auto& use : semantic_uses) {
      const uint32_t usage = use.first >> 8;
      const uint32_t index = use.first & 0xFFu;
      std::cout << "vertex_semantic " << broken_bond::gfx::VertexUsageName(usage) << index << " "
                << use.second << "\n";
    }
    std::cout << "vb_shaders " << translated.vb_shaders << " with_binding " << translated.vb_with_binding
              << " attr_stride " << translated.vb_attr_stride << " float " << translated.vb_float
              << " index0 " << translated.vb_index0 << "\n";
    std::cout << "disasm vfetch " << translated.disasm_vfetch << " format " << translated.disasm_format
              << " stride " << translated.disasm_stride << "\n";
    if (!translated.disasm_sample.empty()) {
      std::cout << "--- vs sample ---\n" << translated.disasm_sample << "\n--- sample dwords ---\n";
      for (uint32_t i = 0; i < translated.sample_dword_count; ++i) {
        std::cout << std::hex << translated.sample_dwords[i] << (i + 1 == translated.sample_dword_count ? "" : " ");
      }
      std::cout << std::dec << "\n";
    }
    std::cout << "vfetch example format " << translated.example_format << " stride "
              << translated.example_stride << " offset " << translated.example_offset << " fetch "
              << translated.example_fetch << " index " << translated.example_index << " swizzle "
              << translated.example_swizzle << "\n";
    for (uint32_t format = 0; format < 64; ++format) {
      if (translated.format_hits[format] != 0) {
        std::cout << "vfetch format " << format << " " << translated.format_hits[format] << "\n";
      }
    }
    draw_batch(translated.guest_alu, "guest_alu");
    draw_batch(translated.guest_textured, "guest_tex");
    draw_batch(translated.guest_interpolated, "guest_interp");
    draw_batch(translated.guest_vertices, "guest_vert");
    draw_batch(translated.guest_declared, "guest_decl");
    draw_batch(translated.guest_packed, "guest_pack");
    if (pack_seen == 0 || pack_bad != 0 || slot_bad != 0) {
      guest_draw_failed = 1;
    }
    draw_batch(translated.guest_short, "guest_short");
    uint32_t link_full = 0;
    if (!guest_open || link_fragment.empty() || translated.guest_declared.empty()) {
      std::cout << "guest_link missing\n";
      guest_draw_failed = 1;
    } else {
      for (const TranslateStats::GuestDraw& vertex : translated.guest_declared) {
        std::vector<uint8_t> passed;
        std::vector<uint8_t> paired;
        const bool pass_ok = device.DrawGuestFragment(
            vertex.words.data(), vertex.words.size(), kGuestSize, kGuestSize, 0, 0, 0, false, 0.f,
            true, vertex.streams.data(), uint32_t(vertex.streams.size()), passed, guest_reason);
        const bool pair_ok =
            pass_ok &&
            device.DrawGuestFragment(vertex.words.data(), vertex.words.size(), kGuestSize, kGuestSize,
                                    0, 0, 0, false, 0.f, true, vertex.streams.data(),
                                    uint32_t(vertex.streams.size()), paired, guest_reason,
                                    link_fragment.data(), link_fragment.size());
        uint32_t matched = 0;
        if (pair_ok) {
          for (size_t i = 0; i + 3 < passed.size() && i + 3 < paired.size(); i += 4) {
            if (passed[i] == paired[i] && passed[i + 1] == paired[i + 1] &&
                passed[i + 2] == paired[i + 2] && passed[i + 3] == paired[i + 3]) {
              ++matched;
            }
          }
        }
        std::cout << "guest_link " << (pair_ok ? "ok" : "failed");
        if (!pair_ok) {
          std::cout << " " << guest_reason;
        }
        std::cout << " matched " << matched;
        if (paired.size() >= 4) {
          std::cout << " first " << int(paired[0]) << " " << int(paired[1]) << " " << int(paired[2])
                    << " " << int(paired[3]);
        }
        std::cout << "\n";
        if (matched == kGuestSize * kGuestSize) {
          ++link_full;
        }
      }
      if (link_full != translated.guest_declared.size()) {
        guest_draw_failed = 1;
      }
    }
    if (guest_draws == 0) {
      guest_draw_failed = 1;
    }
#endif
#endif
    if (tiling_failed != 0 || guest_draw_failed != 0 || semantic_failed != 0) {
      return 1;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
