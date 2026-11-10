#include "web/pipeline_manifest.h"

#include <cstdio>
#include <iostream>

namespace broken_bond::web {
namespace {

constexpr uint32_t kMagic = 0x4F535057u;
constexpr uint32_t kVersion = 1;
constexpr const char* kPath = "/opfs/broken_bond/pipeline_manifest.bin";

bool WriteU32(FILE* file, uint32_t value) { return std::fwrite(&value, 4, 1, file) == 1; }
bool WriteU64(FILE* file, uint64_t value) { return std::fwrite(&value, 8, 1, file) == 1; }
bool ReadU32(FILE* file, uint32_t& value) { return std::fread(&value, 4, 1, file) == 1; }
bool ReadU64(FILE* file, uint64_t& value) { return std::fread(&value, 8, 1, file) == 1; }

}  // namespace

PipelineManifest& Manifest() {
  static PipelineManifest manifest;
  return manifest;
}

bool PipelineManifest::HasScene(const PipelineRecord& record, uint64_t scene) const {
  for (uint32_t i = 0; i < record.scene_count && i < 4; ++i) {
    if (record.scenes[i] == scene) return true;
  }
  return false;
}

void PipelineManifest::AddScene(PipelineRecord& record, uint64_t scene) {
  if (scene == 0 || HasScene(record, scene) || record.scene_count >= 4) return;
  record.scenes[record.scene_count++] = scene;
  dirty_ = true;
}

uint32_t PipelineManifest::pending() const {
  uint32_t count = 0;
  for (const PipelineRecord& record : records_) {
    if (!record.built && !record.failed) ++count;
  }
  return count;
}

uint32_t PipelineManifest::menu_count() const {
  uint32_t count = 0;
  for (const PipelineRecord& record : records_) {
    if (record.tier == 0) ++count;
  }
  return count;
}

bool PipelineManifest::Load() {
  FILE* file = std::fopen(kPath, "rb");
  if (file == nullptr) return false;
  uint32_t magic = 0, version = 0, count = 0, locked = 0;
  uint64_t menu = 0;
  const bool header = ReadU32(file, magic) && ReadU32(file, version) && ReadU32(file, count) &&
                      ReadU32(file, locked) && ReadU64(file, menu);
  if (!header || magic != kMagic || version != kVersion || count > 100000) {
    std::fclose(file);
    return false;
  }
  std::vector<PipelineRecord> loaded;
  loaded.reserve(count);
  bool ok = true;
  for (uint32_t n = 0; ok && n < count; ++n) {
    PipelineRecord record;
    ok = ReadU64(file, record.key) && ReadU64(file, record.vs) && ReadU64(file, record.ps);
    for (uint32_t s = 0; ok && s < 4; ++s) ok = ReadU64(file, record.scenes[s]);
    ok = ok && ReadU32(file, record.scene_count) && ReadU32(file, record.tier) && ReadU32(file, record.spec) &&
         ReadU32(file, record.topology) && ReadU32(file, record.samples) && ReadU32(file, record.depth_enabled) &&
         ReadU32(file, record.depth_write) && ReadU32(file, record.depth_function) &&
         ReadU32(file, record.stencil_enabled) && ReadU32(file, record.stencil_ref) &&
         ReadU32(file, record.stencil_read) && ReadU32(file, record.stencil_write) &&
         ReadU32(file, record.front_pass) && ReadU32(file, record.front_fail) &&
         ReadU32(file, record.front_depth_fail) && ReadU32(file, record.front_compare) &&
         ReadU32(file, record.back_pass) && ReadU32(file, record.back_fail) &&
         ReadU32(file, record.back_depth_fail) && ReadU32(file, record.back_compare) && ReadU32(file, record.cull) &&
         ReadU32(file, record.front) && ReadU32(file, record.blend_enabled) && ReadU32(file, record.write_mask) &&
         ReadU32(file, record.src_blend) && ReadU32(file, record.dst_blend) && ReadU32(file, record.blend_op) &&
         ReadU32(file, record.src_blend_alpha) && ReadU32(file, record.dst_blend_alpha) &&
         ReadU32(file, record.blend_op_alpha);
    uint32_t bias_bits = 0;
    ok = ok && ReadU32(file, bias_bits) && ReadU32(file, record.slope_bits) && ReadU32(file, record.ps_present) &&
         ReadU32(file, record.element_count) && ReadU32(file, record.slot_count);
    record.depth_bias = static_cast<int32_t>(bias_bits);
    if (record.scene_count > 4 || record.element_count > 16 || record.slot_count > 16) ok = false;
    record.elements.resize(record.element_count);
    for (uint32_t e = 0; ok && e < record.element_count; ++e) {
      PipelineElement& element = record.elements[e];
      ok = ReadU32(file, element.index) && ReadU32(file, element.used) && ReadU32(file, element.location) &&
           ReadU32(file, element.format) && ReadU32(file, element.slot) && ReadU32(file, element.offset) &&
           ReadU32(file, element.usage) && ReadU32(file, element.usage_index);
    }
    record.strides.resize(record.slot_count);
    for (uint32_t s = 0; ok && s < record.slot_count; ++s) ok = ReadU32(file, record.strides[s]);
    if (ok) loaded.push_back(std::move(record));
  }
  std::fclose(file);
  if (!ok || loaded.size() != count) return false;
  records_ = std::move(loaded);
  index_.clear();
  for (size_t i = 0; i < records_.size(); ++i) index_[records_[i].key] = i;
  menu_locked_ = locked != 0;
  menu_scene_ = menu;
  dirty_ = false;
  std::cout << "web pipelines: restored " << records_.size() << ", menu " << menu_count() << std::endl;
  return true;
}

void PipelineManifest::Save() {
  if (!dirty_) return;
  const std::string tmp = std::string(kPath) + ".tmp";
  FILE* file = std::fopen(tmp.c_str(), "wb");
  if (file == nullptr) return;
  const uint32_t count = uint32_t(records_.size());
  bool ok = WriteU32(file, kMagic) && WriteU32(file, kVersion) && WriteU32(file, count) &&
            WriteU32(file, menu_locked_ ? 1u : 0u) && WriteU64(file, menu_scene_);
  for (const PipelineRecord& record : records_) {
    ok = ok && WriteU64(file, record.key) && WriteU64(file, record.vs) && WriteU64(file, record.ps);
    for (uint32_t s = 0; s < 4; ++s) ok = ok && WriteU64(file, record.scenes[s]);
    ok = ok && WriteU32(file, record.scene_count) && WriteU32(file, record.tier) && WriteU32(file, record.spec) &&
         WriteU32(file, record.topology) && WriteU32(file, record.samples) && WriteU32(file, record.depth_enabled) &&
         WriteU32(file, record.depth_write) && WriteU32(file, record.depth_function) &&
         WriteU32(file, record.stencil_enabled) && WriteU32(file, record.stencil_ref) &&
         WriteU32(file, record.stencil_read) && WriteU32(file, record.stencil_write) &&
         WriteU32(file, record.front_pass) && WriteU32(file, record.front_fail) &&
         WriteU32(file, record.front_depth_fail) && WriteU32(file, record.front_compare) &&
         WriteU32(file, record.back_pass) && WriteU32(file, record.back_fail) &&
         WriteU32(file, record.back_depth_fail) && WriteU32(file, record.back_compare) && WriteU32(file, record.cull) &&
         WriteU32(file, record.front) && WriteU32(file, record.blend_enabled) && WriteU32(file, record.write_mask) &&
         WriteU32(file, record.src_blend) && WriteU32(file, record.dst_blend) && WriteU32(file, record.blend_op) &&
         WriteU32(file, record.src_blend_alpha) && WriteU32(file, record.dst_blend_alpha) &&
         WriteU32(file, record.blend_op_alpha) && WriteU32(file, static_cast<uint32_t>(record.depth_bias)) &&
         WriteU32(file, record.slope_bits) && WriteU32(file, record.ps_present) &&
         WriteU32(file, record.element_count) && WriteU32(file, record.slot_count);
    for (uint32_t e = 0; ok && e < record.element_count && e < record.elements.size(); ++e) {
      const PipelineElement& element = record.elements[e];
      ok = WriteU32(file, element.index) && WriteU32(file, element.used) && WriteU32(file, element.location) &&
           WriteU32(file, element.format) && WriteU32(file, element.slot) && WriteU32(file, element.offset) &&
           WriteU32(file, element.usage) && WriteU32(file, element.usage_index);
    }
    for (uint32_t s = 0; ok && s < record.slot_count && s < record.strides.size(); ++s) {
      ok = WriteU32(file, record.strides[s]);
    }
  }
  std::fclose(file);
  if (!ok) return;
  std::remove(kPath);
  if (std::rename(tmp.c_str(), kPath) == 0) dirty_ = false;
}

PipelineRecord* PipelineManifest::Note(PipelineRecord incoming) {
  const uint64_t key = incoming.key;
  if (const auto found = index_.find(key); found != index_.end() && found->second < records_.size()) {
    PipelineRecord& have = records_[found->second];
    AddScene(have, incoming.scenes[0]);
    if (menu_locked_ && HasScene(have, menu_scene_) && have.tier != 0) {
      have.tier = 0;
      dirty_ = true;
    }
    return &have;
  }
  if (incoming.scene_count == 0 && incoming.scenes[0] != 0) incoming.scene_count = 1;
  if (menu_locked_ && HasScene(incoming, menu_scene_)) incoming.tier = 0;
  const size_t index = records_.size();
  records_.push_back(std::move(incoming));
  index_[key] = index;
  dirty_ = true;
  return &records_.back();
}

void PipelineManifest::Touch(uint64_t key, uint64_t scene) {
  const auto found = index_.find(key);
  if (found == index_.end() || found->second >= records_.size()) return;
  PipelineRecord& have = records_[found->second];
  AddScene(have, scene);
  if (menu_locked_ && HasScene(have, menu_scene_) && have.tier != 0) {
    have.tier = 0;
    dirty_ = true;
  }
}

PipelineRecord* PipelineManifest::Next(uint64_t hot_scene) {
  PipelineRecord* menu = nullptr;
  PipelineRecord* scene = nullptr;
  PipelineRecord* rest = nullptr;
  for (PipelineRecord& record : records_) {
    if (record.built || record.failed) continue;
    if (record.tier == 0) {
      if (menu == nullptr) menu = &record;
    } else if (hot_scene != 0 && HasScene(record, hot_scene)) {
      if (scene == nullptr) scene = &record;
    } else if (rest == nullptr) {
      rest = &record;
    }
    if (menu != nullptr && scene != nullptr) break;
  }
  if (menu != nullptr) return menu;
  if (scene != nullptr) return scene;
  return rest;
}

void PipelineManifest::FinishFrame(uint64_t scene, uint32_t draws) {
  if (scene != 0) hot_scene_ = scene;
  if (draws < 150 || scene == 0) return;
  if (menu_locked_) {
    if (scene == menu_scene_) {
      for (PipelineRecord& record : records_) {
        if (HasScene(record, scene) && record.tier != 0) {
          record.tier = 0;
          dirty_ = true;
        }
      }
    }
    return;
  }
  if (scene == menu_candidate_) {
    ++menu_run_;
  } else {
    menu_candidate_ = scene;
    menu_run_ = 1;
  }
  if (menu_run_ < 20) return;
  menu_locked_ = true;
  menu_scene_ = scene;
  for (PipelineRecord& record : records_) {
    if (HasScene(record, scene)) record.tier = 0;
  }
  dirty_ = true;
  if (!logged_menu_) {
    logged_menu_ = true;
    std::cout << "web pipelines: menu scene locked, " << menu_count() << " pipelines" << std::endl;
    Save();
  }
}

}  // namespace broken_bond::web
