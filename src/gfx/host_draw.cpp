#include "gfx/host_draw.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/string/buffer.h>

REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Unused here. AnalyzeUcode reads it.");

namespace rex::graphics::draw_util {
extern const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
extern const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
}  // namespace rex::graphics::draw_util

#include "gfx/device.h"
#include "gfx/device_shadow.h"
#include "gfx/shader_object.h"
#include "gfx/texture.h"
#include "gfx/vertex_semantic.h"

namespace broken_bond::gfx {
namespace {

uint32_t LoadBe(const uint8_t* image, uint32_t at) {
  return (uint32_t(image[at]) << 24) | (uint32_t(image[at + 1]) << 16) |
         (uint32_t(image[at + 2]) << 8) | uint32_t(image[at + 3]);
}

void StoreBe(std::vector<uint8_t>& image, uint32_t at, uint32_t value) {
  image[at] = static_cast<uint8_t>(value >> 24);
  image[at + 1] = static_cast<uint8_t>(value >> 16);
  image[at + 2] = static_cast<uint8_t>(value >> 8);
  image[at + 3] = static_cast<uint8_t>(value);
}

uint32_t HostFromBe(uint32_t loaded) {
  return (loaded >> 24) | ((loaded >> 8) & 0x0000FF00u) | ((loaded << 8) & 0x00FF0000u) |
         (loaded << 24);
}

uint64_t Mix(uint64_t hash, const uint8_t* bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

struct TranslatedDraw {
  std::vector<uint32_t> vertex_words;
  std::vector<uint32_t> pixel_words;
  uint32_t fetch_constant = 0;
  uint32_t image_count = 0;
  uint32_t sampler_count = 0;
  bool pad_position_w = false;
  uint32_t stride_words = 0;
};

std::unordered_map<uint64_t, TranslatedDraw> g_translated;

bool TranslatePair(Device& device, std::vector<uint32_t>& patched,
                   const std::vector<uint8_t>& pixel_raw, TranslatedDraw& program,
                   std::string& reason) {
  const DeviceInfo& info = device.info();
  rex::graphics::SpirvShaderTranslator::Features features(false);
  if (info.api >= VK_API_VERSION_1_2) {
    features.spirv_version = spv::Spv_1_5;
    features.max_storage_buffer_range = info.storage_range;
    features.signed_zero_inf_nan_preserve_float32 = info.float_preserve;
    features.denorm_flush_to_zero_float32 = info.float_flush;
    features.rounding_mode_rte_float32 = info.float_rte;
    features.demote_to_helper_invocation = info.demote;
    features.sample_rate_shading = info.sample_rate;
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
      reason = "host shader did not translate";
      return false;
    }
    const std::vector<uint8_t>& binary = translation.translated_binary();
    if (binary.size() < 4 || binary.size() % 4 != 0) {
      reason = "host shader is not a SPIR-V module";
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
  if (!translate(true, mask, vertex_shader, program.vertex_words)) {
    return false;
  }
  rex::graphics::SpirvShader pixel_shader(rex::graphics::xenos::ShaderType::kPixel, 0,
                                         reinterpret_cast<const uint32_t*>(pixel_raw.data()),
                                         uint32_t(pixel_raw.size() / 4u), std::endian::big);
  if (!translate(false, mask, pixel_shader, program.pixel_words)) {
    return false;
  }
  if (vertex_shader.vertex_bindings().empty()) {
    reason = "host vertex shader has no fetch";
    return false;
  }
  program.fetch_constant = vertex_shader.vertex_bindings()[0].fetch_constant;
  program.image_count = uint32_t(pixel_shader.GetTextureBindingsAfterTranslation().size());
  program.sampler_count = uint32_t(pixel_shader.GetSamplerBindingsAfterTranslation().size());
  return true;
}

}  // namespace

bool SubmitRetainedDraw(Device& device, const uint8_t* image, size_t image_bytes,
                        uint32_t image_device, bool allow_new, bool sample, RetainedDrawInfo& info,
                        std::string& reason) {
  info = {};
  if (image == nullptr || image_bytes < 12492u) {
    reason = "host draw image is short";
    return false;
  }
  DeviceDraw draw;
  std::vector<uint8_t> stream_bytes;
  std::vector<VertexElement> elements;
  ShaderMicrocode vertex_ucode;
  ShaderMicrocode pixel_ucode;
  std::vector<uint8_t> vertex_raw;
  std::vector<uint8_t> pixel_raw;
  std::vector<float> floats;
  if (!ReadDeviceDraw(image, image_bytes, image_device, draw, stream_bytes, reason) ||
      !ReadDeviceDeclaration(image, image_bytes, image_device, elements, reason) ||
      !ReadVertexShaderMicrocode(image, image_bytes, draw.vertex_shader, vertex_ucode, vertex_raw,
                                 reason) ||
      !ReadPixelShaderMicrocode(image, image_bytes, draw.pixel_shader, pixel_ucode, pixel_raw,
                                reason) ||
      !ReadDeviceFloats(image, image_bytes, image_device, floats, reason)) {
    if (reason.empty()) {
      reason = "host draw was not read";
    }
    return false;
  }
  if (vertex_raw.size() < 12 || vertex_raw.size() % 4 != 0 || pixel_raw.size() < 12 ||
      floats.size() < 2048u || elements.empty() || draw.stream_stride_dwords == 0) {
    reason = "host draw is missing a shader, a declaration, or vertices";
    return false;
  }
  const uint32_t index_at = LoadBe(image, 12436u);
  const uint32_t index_bytes = LoadBe(image, 12440u);
  const uint32_t index_size = LoadBe(image, 12444u);
  const uint32_t index_first = LoadBe(image, 12448u);
  std::vector<uint16_t> indices;
  if (index_bytes != 0) {
    if ((index_size != 2u && index_size != 4u) || index_bytes < index_size ||
        (index_bytes % index_size) != 0 || uint64_t(index_at) + index_bytes > image_bytes) {
      reason = "host draw index list is not 16-bit or 32-bit";
      return false;
    }
    indices.resize(index_bytes / index_size);
    for (uint32_t i = 0; i < indices.size(); ++i) {
      const uint32_t at = index_at + i * index_size;
      uint32_t index = index_size == 2u ? (uint32_t(image[at]) << 8) | uint32_t(image[at + 1])
                                        : LoadBe(image, at);
      if (index == 0xFFFFu || index == 0xFFFFFFFFu) {
        index = 0;
      } else if (index >= index_first) {
        index -= index_first;
      }
      if (index > 0xFFFFu) {
        reason = "host draw index does not fit in 16 bits";
        return false;
      }
      indices[i] = static_cast<uint16_t>(index);
    }
  }
  const uint32_t vertex_object = draw.vertex_shader & 0x1FFFFFFFu;
  int marker = -1;
  if (vertex_object + 0x800u <= image_bytes) {
    for (uint32_t at = vertex_object; at + 4u <= vertex_object + 0x800u; at += 4u) {
      if (LoadBe(image, at) == 0x290u) {
        marker = static_cast<int>(at);
        break;
      }
    }
  }
  if (marker < 0) {
    reason = "host vertex shader has no semantic marker";
    return false;
  }
  uint64_t key = 14695981039346656037ull;
  key = Mix(key, vertex_raw.data(), vertex_raw.size());
  key = Mix(key, pixel_raw.data(), pixel_raw.size());
  for (const VertexElement& element : elements) {
    uint32_t packed = element.format ^ (element.usage << 8) ^ (element.usage_index << 16);
    key = Mix(key, reinterpret_cast<const uint8_t*>(&packed), sizeof(packed));
  }
  const auto found = g_translated.find(key);
  info.from_cache = found != g_translated.end();
  if (!info.from_cache && !allow_new) {
    reason = "deferred host shader";
    return false;
  }
  TranslatedDraw local;
  TranslatedDraw* program = nullptr;
  if (info.from_cache) {
    program = &found->second;
  } else {
    std::vector<uint8_t> container(0x100u + vertex_raw.size(), 0);
    StoreBe(container, 4u, 0x100u);
    StoreBe(container, 24u, 0x60u);
    StoreBe(container, 48u, 0xFFFE0300u);
    StoreBe(container, 0x60u, 0u);
    StoreBe(container, 0x64u, uint32_t(vertex_raw.size()));
    const uint32_t blob = std::min(32u, uint32_t(image_bytes - uint32_t(marker)) / 4u);
    for (uint32_t i = 0; i < blob; ++i) {
      StoreBe(container, 0x68u + i * 4u, LoadBe(image, uint32_t(marker) + i * 4u));
    }
    std::memcpy(container.data() + 0x100u, vertex_raw.data(), vertex_raw.size());
    std::vector<VertexFetchSemantic> semantics;
    if (!ReadVertexFetchSemantics(container.data(), container.size(), semantics, reason)) {
      if (reason.empty()) {
        reason = "host vertex semantics were not read";
      }
      return false;
    }
    std::vector<uint32_t> patched(vertex_raw.size() / 4u);
    for (uint32_t i = 0; i < patched.size(); ++i) {
      uint32_t loaded = 0;
      std::memcpy(&loaded, vertex_raw.data() + i * 4u, 4u);
      patched[i] = HostFromBe(loaded);
    }
    local.pad_position_w = elements[0].format == 57u;
    local.stride_words = draw.stream_stride_dwords;
    if (local.pad_position_w) {
      elements[0].format = 38;
      for (VertexElement& element : elements) {
        if (element.format == 37u || element.format == 38u || element.format == 57u) {
          element.integer_components = false;
        }
        if (element.offset_words >= 3) {
          element.offset_words += 1;
        }
        element.stride_words += 1;
      }
      local.stride_words += 1u;
    }
    if (!PatchVertexDeclaration(patched.data(), uint32_t(patched.size()), semantics.data(),
                                uint32_t(semantics.size()), elements.data(),
                                uint32_t(elements.size()), reason)) {
      if (reason.empty()) {
        reason = "host declaration was not patched";
      }
      return false;
    }
    if (!TranslatePair(device, patched, pixel_raw, local, reason)) {
      return false;
    }
    program = &g_translated.emplace(key, std::move(local)).first->second;
  }

  uint32_t texture_offset = LoadBe(image, 12456u);
  uint32_t texture_bytes = LoadBe(image, 12460u);
  uint32_t texture_format = LoadBe(image, 12464u);
  uint32_t texture_width_px = LoadBe(image, 12468u);
  uint32_t texture_height_px = LoadBe(image, 12472u);
  uint32_t texture_pitch = LoadBe(image, 12476u);
  uint32_t texture_endian = LoadBe(image, 12480u);
  uint32_t texture_tiled = LoadBe(image, 12484u);
  const bool texture_tail = texture_bytes != 0 && texture_offset <= image_bytes &&
                            texture_bytes <= image_bytes - texture_offset &&
                            texture_offset + texture_bytes == image_bytes &&
                            (texture_format == 6u || texture_format == 18u || texture_format == 20u);
  if (program->image_count != 0 && !texture_tail) {
    reason = texture_format == 0 ? "host texture is absent"
                                 : "host texture format is " + std::to_string(texture_format);
    return false;
  }
  std::vector<uint8_t> rgba;
  if (texture_tail &&
      !LoadGuestTextureRgba8(texture_format, texture_width_px, texture_height_px, texture_pitch,
                             texture_endian, texture_tiled != 0, image + texture_offset,
                             texture_bytes, rgba, reason)) {
    if (reason.empty()) {
      reason = "host texture was not decoded";
    }
    return false;
  }
  const uint32_t endian = texture_endian == 1u || texture_endian == 2u ? texture_endian : 1u;
  std::vector<uint8_t> padded;
  const uint8_t* vertex_bytes = stream_bytes.data();
  uint32_t vertex_byte_count = uint32_t(stream_bytes.size());
  if (program->pad_position_w && program->stride_words != 0) {
    const uint32_t dst_stride = program->stride_words * 4u;
    const uint32_t src_stride = dst_stride - 4u;
    if (src_stride == 0 || stream_bytes.size() % src_stride != 0) {
      reason = "host position pad does not match the stream";
      return false;
    }
    const uint32_t verts = uint32_t(stream_bytes.size() / src_stride);
    padded.resize(static_cast<size_t>(verts) * dst_stride);
    const uint32_t w_bits = endian == 1u ? 0x803f0000u : endian == 2u ? 0x0000803fu : 0x3f800000u;
    for (uint32_t v = 0; v < verts; ++v) {
      const uint8_t* src = stream_bytes.data() + static_cast<size_t>(v) * src_stride;
      uint8_t* dst = padded.data() + static_cast<size_t>(v) * dst_stride;
      std::memcpy(dst, src, 12);
      std::memcpy(dst + 12, &w_bits, 4);
      if (src_stride > 12u) {
        std::memcpy(dst + 16, src + 12, src_stride - 12u);
      }
    }
    vertex_bytes = padded.data();
    vertex_byte_count = uint32_t(padded.size());
  }
  VertexStream stream;
  stream.fetch_constant = program->fetch_constant;
  stream.stride_words = program->stride_words;
  stream.bytes = vertex_bytes;
  stream.byte_count = vertex_byte_count;
  stream.indices = indices.empty() ? nullptr : indices.data();
  stream.index_count = uint32_t(indices.size());
  stream.endian = endian;
  Device::RasterState raster;
  raster.surface_info = draw.surface_info & ~0x30000u;
  raster.depth_control = draw.depth_control;
  raster.blend_control = draw.blend_control;
  raster.mode_cntl = draw.mode_cntl;
  raster.clear_red = 1.f;
  raster.clear_green = 0.f;
  raster.clear_blue = 1.f;
  raster.clear_alpha = 1.f;
  raster.retain_frame = true;
  raster.readback = sample;
  std::vector<uint8_t> pixels;
  if (!device.DrawGuestFragment(program->vertex_words.data(), program->vertex_words.size(), 1280,
                                512, program->image_count, program->sampler_count, 1, false, 0.f,
                                true, &stream, 1, pixels, reason, program->pixel_words.data(),
                                program->pixel_words.size(), rgba.empty() ? nullptr : rgba.data(),
                                rgba.empty() ? 0u : texture_width_px,
                                rgba.empty() ? 0u : texture_height_px, &raster, floats.data(),
                                floats.data() + 1024)) {
    if (reason.empty()) {
      reason = "host draw failed";
    }
    return false;
  }
  if (sample) {
    info.sampled = true;
    std::unordered_map<uint32_t, uint32_t> seen;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      if (pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 255 && pixels[i + 3] == 255) {
        continue;
      }
      const uint32_t rgb = (uint32_t(pixels[i]) << 16) | (uint32_t(pixels[i + 1]) << 8) |
                           uint32_t(pixels[i + 2]);
      seen.emplace(rgb, 1);
      ++info.covered;
    }
    info.colors = uint32_t(seen.size());
  }
  return true;
}

}  // namespace broken_bond::gfx
