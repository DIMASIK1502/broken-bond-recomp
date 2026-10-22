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

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/string/buffer.h>

#if defined(SHADER_DB_VULKAN_SDK)
#include <spirv-tools/libspirv.h>
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
#if defined(SHADER_DB_VULKAN_SDK)
  spv_context context = nullptr;
  using ContextCreate = spv_context (*)(spv_target_env);
  using ContextDestroy = void (*)(spv_context);
  using ValidateBinary = spv_result_t (*)(const spv_const_context, const uint32_t*, size_t,
                                         spv_diagnostic*);
  using DiagnosticDestroy = void (*)(spv_diagnostic);
  ContextDestroy destroy_context = nullptr;
  ValidateBinary validate = nullptr;
  DiagnosticDestroy destroy_diagnostic = nullptr;
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
    if (destroy_device != nullptr && device != VK_NULL_HANDLE) {
      destroy_device(device, nullptr);
    }
    if (destroy_instance != nullptr && instance != VK_NULL_HANDLE) {
      destroy_instance(instance, nullptr);
    }
#if defined(SHADER_DB_VULKAN_SDK)
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
  void Check(const std::vector<uint8_t>& binary);
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
    validate = reinterpret_cast<ValidateBinary>(GetProcAddress(tools, "spvValidateBinary"));
    destroy_diagnostic =
        reinterpret_cast<DiagnosticDestroy>(GetProcAddress(tools, "spvDiagnosticDestroy"));
    if (create_context == nullptr || destroy_context == nullptr || validate == nullptr ||
        destroy_diagnostic == nullptr) {
      validator_reason = "SPIRV-Tools exports are missing";
    } else {
      context = create_context(SPV_ENV_VULKAN_1_2);
      if (context == nullptr) {
        validator_reason = "spvContextCreate failed";
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

  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  VkPhysicalDeviceFeatures2 features{};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  if (api >= VK_API_VERSION_1_3) {
    features.pNext = &features13;
  }
  get_features(physical, &features);
  sample_rate = features.features.sampleRateShading == VK_TRUE;
  demote = api >= VK_API_VERSION_1_3 && features13.shaderDemoteToHelperInvocation == VK_TRUE;

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
  VkPhysicalDeviceVulkan13Features enabled13{};
  enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  enabled13.shaderDemoteToHelperInvocation = demote ? VK_TRUE : VK_FALSE;
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue;
  device_info.pEnabledFeatures = &enabled;
  if (demote) {
    device_info.pNext = &enabled13;
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
  if (destroy_device == nullptr || create_module == nullptr || destroy_module == nullptr) {
    vulkan_reason = "device exports are missing";
  }
}

void HostProbe::Check(const std::vector<uint8_t>& binary) {
  const auto* words = reinterpret_cast<const uint32_t*>(binary.data());
  const size_t word_count = binary.size() / 4;
#if defined(SHADER_DB_VULKAN_SDK)
  if (context != nullptr && validate != nullptr) {
    spv_diagnostic diagnostic = nullptr;
    const spv_result_t result = validate(context, words, word_count, &diagnostic);
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

void TranslateShader(rex::graphics::SpirvShaderTranslator& translator, uint32_t token,
                     const UcodeSpan& ucode, TranslateStats& stats, HostProbe& probe) {
  const auto type = token == 0xFFFE0300u ? rex::graphics::xenos::ShaderType::kVertex
                                         : rex::graphics::xenos::ShaderType::kPixel;
  rex::graphics::SpirvShader shader(type, 0, ucode.dwords, ucode.dword_count, std::endian::big);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
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
      probe.Check(binary);
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

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: shader_db <xeshader.sdb>\n";
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

    uint32_t vertex = 0;
    uint32_t pixel = 0;
    uint32_t other = 0;
    uint64_t raw_bytes = 0;
    uint64_t ucode_bytes = 0;
    uint32_t tag_count[3] = {};
    ConstantStats constants;
#if defined(SHADER_DB_TRANSLATE)
    rex::graphics::SpirvShaderTranslator::Features features(true);
    rex::graphics::SpirvShaderTranslator translator(features, false, false, false);
    TranslateStats translated;
    HostProbe probe;
    probe.Open();
#endif
    for (const ShaderRecord& shader : shaders) {
      const std::vector<uint8_t> object =
          InflateZlib(file.data() + shader.offset, shader.compressed_size, shader.raw_size);
      raw_bytes += object.size();
      if (object.size() < 52) {
        Fail("shader object is shorter than its version token");
      }
      const uint32_t token = ReadBe32(object.data() + 48);
      const UcodeSpan ucode = LocateUcode(object.data(), object.size(), token);
      ucode_bytes += uint64_t(ucode.dword_count) * 4;
      ReadConstants(object.data(), object.size(), token, constants);
#if defined(SHADER_DB_TRANSLATE)
      TranslateShader(translator, token, ucode, translated, probe);
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
      std::cout << "validator vulkan-1.2\n";
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
      std::cout << "  modules " << (probe.module_ok + probe.module_failed) << "\n";
      std::cout << "  ok " << probe.module_ok << "\n";
      std::cout << "  failed " << probe.module_failed << "\n";
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
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
