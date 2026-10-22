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
//   each payload is zlib. Dword at offset 48 of the inflated object is the
//   D3D9 shader version: 0xFFFE0300 is vs_3_0, 0xFFFF0300 is ps_3_0.

#include <cstdint>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
      Fail(std::string("cannot open ") + argv[1]);
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
    uint32_t tag_count[3] = {};
    for (const ShaderRecord& shader : shaders) {
      const std::vector<uint8_t> object =
          InflateZlib(file.data() + shader.offset, shader.compressed_size, shader.raw_size);
      raw_bytes += object.size();
      if (object.size() < 52) {
        Fail("shader object is shorter than its version token");
      }
      const uint32_t token = ReadBe32(object.data() + 48);
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
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
