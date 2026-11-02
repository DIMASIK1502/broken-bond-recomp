// Pipelines the browser has already been asked to draw, so the next launch can
// build them before the guest reaches that scene. The record is the host
// descriptor plus the WGSL cache keys. Chrome's compiled code stays in
// Chrome's own cache; an identical descriptor is what makes that cache hit.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace broken_bond::web {

struct PipelineElement {
  uint32_t index = 0;
  uint32_t used = 0;
  uint32_t location = 0;
  uint32_t format = 0;
  uint32_t slot = 0;
  uint32_t offset = 0;
  uint32_t usage = 0;
  uint32_t usage_index = 0;
};

struct PipelineRecord {
  uint64_t key = 0;
  uint64_t vs = 0;
  uint64_t ps = 0;
  uint64_t scenes[4] = {};
  uint32_t scene_count = 0;
  uint32_t tier = 1;
  uint32_t spec = 0;
  uint32_t topology = 0;
  uint32_t samples = 1;
  uint32_t depth_enabled = 0;
  uint32_t depth_write = 0;
  uint32_t depth_function = 0;
  uint32_t stencil_enabled = 0;
  uint32_t stencil_ref = 0;
  uint32_t stencil_read = 0;
  uint32_t stencil_write = 0;
  uint32_t front_pass = 0;
  uint32_t front_fail = 0;
  uint32_t front_depth_fail = 0;
  uint32_t front_compare = 0;
  uint32_t back_pass = 0;
  uint32_t back_fail = 0;
  uint32_t back_depth_fail = 0;
  uint32_t back_compare = 0;
  uint32_t cull = 0;
  uint32_t front = 0;
  uint32_t blend_enabled = 0;
  uint32_t write_mask = 0;
  uint32_t src_blend = 0;
  uint32_t dst_blend = 0;
  uint32_t blend_op = 0;
  uint32_t src_blend_alpha = 0;
  uint32_t dst_blend_alpha = 0;
  uint32_t blend_op_alpha = 0;
  int32_t depth_bias = 0;
  uint32_t slope_bits = 0;
  uint32_t ps_present = 0;
  uint32_t element_count = 0;
  uint32_t slot_count = 0;
  std::vector<PipelineElement> elements;
  std::vector<uint32_t> strides;
  bool built = false;
  bool failed = false;
};

class PipelineManifest {
 public:
  bool Load();
  void Save();
  PipelineRecord* Note(PipelineRecord incoming);
  void Touch(uint64_t key, uint64_t scene);
  PipelineRecord* Next(uint64_t hot_scene);
  void FinishFrame(uint64_t scene, uint32_t draws);
  uint32_t unique() const { return uint32_t(records_.size()); }
  uint32_t pending() const;
  uint32_t menu_count() const;
  uint64_t hot_scene() const { return hot_scene_; }
  bool dirty() const { return dirty_; }

 private:
  void AddScene(PipelineRecord& record, uint64_t scene);
  bool HasScene(const PipelineRecord& record, uint64_t scene) const;

  std::vector<PipelineRecord> records_;
  std::unordered_map<uint64_t, size_t> index_;
  bool dirty_ = false;
  bool menu_locked_ = false;
  uint64_t menu_scene_ = 0;
  uint64_t menu_candidate_ = 0;
  uint32_t menu_run_ = 0;
  uint64_t hot_scene_ = 0;
  bool logged_menu_ = false;
};

PipelineManifest& Manifest();

}  // namespace broken_bond::web
