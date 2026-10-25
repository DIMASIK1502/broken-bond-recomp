// One draw's inputs from a guest image of the device shadow. The running
// game is not sampled.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/vertex_semantic.h"

namespace broken_bond::gfx {

struct DeviceDraw {
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t surface_info = 0;
  uint32_t depth_control = 0;
  uint32_t blend_control = 0;
  uint32_t mode_cntl = 0;
  // Guest address of stream 0. The shadow stores the packed word without
  // 0xC0000000; this field puts it back.
  uint32_t stream_address = 0;
  uint32_t stream_bytes = 0;
  uint32_t stream_stride_dwords = 0;
};

// guest is physical memory. device is the guest address of the device.
// Dwords are big-endian. Stream 0 is fetch constant 95, two dwords at
// device+1912: the packed address, then the byte count.
bool ReadDeviceDraw(const uint8_t* guest, size_t guest_bytes, uint32_t device, DeviceDraw& out,
                    std::vector<uint8_t>& stream, std::string& reason);

// The declaration pointer is the dword at device+11820. A zero pointer yields
// no elements. Count is the dword at object+24. Element i is 12 bytes at
// object+52. offset_words is the byte offset shifted right by 2, and
// stride_words is the byte at device+12528 plus the element's stream.
bool ReadDeviceDeclaration(const uint8_t* guest, size_t guest_bytes, uint32_t device,
                           std::vector<VertexElement>& elements, std::string& reason);

}  // namespace broken_bond::gfx
