#include "gfx/device.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <vulkan/vulkan.h>

namespace broken_bond::gfx {

extern const uint32_t kInterpolatorVertex[];
extern const size_t kInterpolatorVertexWords;
extern const uint32_t kPassFragment[];
extern const size_t kPassFragmentWords;

struct Device::Vulkan {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkCommandPool pool = VK_NULL_HANDLE;

  ~Vulkan() {
    if (device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device);
    }
    if (pool != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device, pool, nullptr);
    }
    if (device != VK_NULL_HANDLE) {
      vkDestroyDevice(device, nullptr);
    }
    if (instance != VK_NULL_HANDLE) {
      vkDestroyInstance(instance, nullptr);
    }
  }
};

Device::Device() = default;
Device::~Device() = default;

const DeviceInfo& Device::info() const { return info_; }

bool Device::Open(std::string& reason) {
  vulkan_ = std::make_unique<Vulkan>();
  uint32_t loader_version = VK_API_VERSION_1_0;
  vkEnumerateInstanceVersion(&loader_version);
  if (loader_version < VK_API_VERSION_1_2) {
    reason = "the Vulkan loader is older than 1.2";
    vulkan_.reset();
    return false;
  }
  const uint32_t requested =
      loader_version >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = "broken_bond";
  application.apiVersion = requested;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &application;
  const VkResult created = vkCreateInstance(&instance_info, nullptr, &vulkan_->instance);
  if (created != VK_SUCCESS) {
    reason = "vkCreateInstance returned " + std::to_string(int(created));
    vulkan_.reset();
    return false;
  }

  uint32_t device_count = 0;
  if (vkEnumeratePhysicalDevices(vulkan_->instance, &device_count, nullptr) != VK_SUCCESS ||
      device_count == 0) {
    reason = "no Vulkan device";
    vulkan_.reset();
    return false;
  }
  std::vector<VkPhysicalDevice> devices(device_count);
  vkEnumeratePhysicalDevices(vulkan_->instance, &device_count, devices.data());

  int best_rank = -1;
  uint32_t api = 0;
  for (VkPhysicalDevice candidate : devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(candidate, &properties);
    const int rank = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                     : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                       : 0;
    if (rank > best_rank) {
      best_rank = rank;
      vulkan_->physical = candidate;
      api = properties.apiVersion;
      info_.name = properties.deviceName;
    }
  }
  if (api < VK_API_VERSION_1_2) {
    reason = "the GPU does not support Vulkan 1.2";
    vulkan_.reset();
    return false;
  }
  info_.api = api;

  VkPhysicalDeviceVulkan12Features features12{};
  features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  VkPhysicalDeviceVulkan13Features features13{};
  features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  VkPhysicalDeviceFeatures2 features{};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features.pNext = &features12;
  if (api >= VK_API_VERSION_1_3) {
    features12.pNext = &features13;
  }
  vkGetPhysicalDeviceFeatures2(vulkan_->physical, &features);
  info_.scalar_block = features12.scalarBlockLayout == VK_TRUE;
  info_.standard_layout = features12.uniformBufferStandardLayout == VK_TRUE;
  info_.sample_rate = features.features.sampleRateShading == VK_TRUE;
  info_.demote = api >= VK_API_VERSION_1_3 && features13.shaderDemoteToHelperInvocation == VK_TRUE;
  info_.dynamic_rendering = api >= VK_API_VERSION_1_3 && features13.dynamicRendering == VK_TRUE;
  if (!info_.scalar_block || !info_.standard_layout) {
    reason = "the GPU is missing scalar block layout or standard uniform layout";
    vulkan_.reset();
    return false;
  }

  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(vulkan_->physical, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(vulkan_->physical, &family_count, families.data());
  vulkan_->family = UINT32_MAX;
  for (uint32_t i = 0; i < family_count; ++i) {
    if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
      vulkan_->family = i;
      break;
    }
  }
  if (vulkan_->family == UINT32_MAX) {
    reason = "no graphics queue";
    vulkan_.reset();
    return false;
  }

  const float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{};
  queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue.queueFamilyIndex = vulkan_->family;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures enabled{};
  enabled.sampleRateShading = info_.sample_rate ? VK_TRUE : VK_FALSE;
  VkPhysicalDeviceVulkan12Features enabled12{};
  enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  enabled12.scalarBlockLayout = VK_TRUE;
  enabled12.uniformBufferStandardLayout = VK_TRUE;
  VkPhysicalDeviceVulkan13Features enabled13{};
  enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  enabled13.shaderDemoteToHelperInvocation = info_.demote ? VK_TRUE : VK_FALSE;
  enabled13.dynamicRendering = info_.dynamic_rendering ? VK_TRUE : VK_FALSE;
  if (api >= VK_API_VERSION_1_3) {
    enabled12.pNext = &enabled13;
  }
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.pNext = &enabled12;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue;
  device_info.pEnabledFeatures = &enabled;
  const VkResult device_result =
      vkCreateDevice(vulkan_->physical, &device_info, nullptr, &vulkan_->device);
  if (device_result != VK_SUCCESS) {
    reason = "vkCreateDevice returned " + std::to_string(int(device_result));
    vulkan_.reset();
    return false;
  }
  vkGetDeviceQueue(vulkan_->device, vulkan_->family, 0, &vulkan_->queue);

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = vulkan_->family;
  const VkResult pool_result =
      vkCreateCommandPool(vulkan_->device, &pool_info, nullptr, &vulkan_->pool);
  if (pool_result != VK_SUCCESS) {
    reason = "vkCreateCommandPool returned " + std::to_string(int(pool_result));
    vulkan_.reset();
    return false;
  }
  return true;
}

bool Device::AcceptShader(const uint32_t* words, size_t word_count, std::string& reason) {
  if (vulkan_ == nullptr || vulkan_->device == VK_NULL_HANDLE) {
    reason = "device is not open";
    return false;
  }
  if (words == nullptr || word_count < 5 || words[0] != 0x07230203u) {
    reason = "shader is not SPIR-V";
    return false;
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = word_count * sizeof(uint32_t);
  info.pCode = words;
  VkShaderModule module = VK_NULL_HANDLE;
  const VkResult result = vkCreateShaderModule(vulkan_->device, &info, nullptr, &module);
  if (result != VK_SUCCESS) {
    reason = "vkCreateShaderModule returned " + std::to_string(int(result));
    return false;
  }
  vkDestroyShaderModule(vulkan_->device, module, nullptr);
  return true;
}

namespace {

uint32_t FindMemory(VkPhysicalDevice physical, uint32_t type_bits, VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical, &properties);
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) != 0 &&
        (properties.memoryTypes[i].propertyFlags & required) == required) {
      return i;
    }
  }
  return UINT32_MAX;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
};

void DestroyBuffer(VkDevice device, Buffer& buffer) {
  if (buffer.buffer != VK_NULL_HANDLE) {
    vkDestroyBuffer(device, buffer.buffer, nullptr);
  }
  if (buffer.memory != VK_NULL_HANDLE) {
    vkFreeMemory(device, buffer.memory, nullptr);
  }
  buffer = {};
}

void DestroyImage(VkDevice device, Image& image) {
  if (image.image != VK_NULL_HANDLE) {
    vkDestroyImage(device, image.image, nullptr);
  }
  if (image.memory != VK_NULL_HANDLE) {
    vkFreeMemory(device, image.memory, nullptr);
  }
  image = {};
}

bool MakeBuffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size,
                VkBufferUsageFlags usage, VkMemoryPropertyFlags memory_flags, Buffer& out,
                std::string& reason) {
  VkBufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &info, nullptr, &out.buffer) != VK_SUCCESS) {
    reason = "vkCreateBuffer failed";
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
  const uint32_t type = FindMemory(physical, requirements.memoryTypeBits, memory_flags);
  if (type == UINT32_MAX) {
    reason = "no memory for a transfer buffer";
    return false;
  }
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  if (vkAllocateMemory(device, &allocate, nullptr, &out.memory) != VK_SUCCESS) {
    reason = "vkAllocateMemory failed for a buffer";
    return false;
  }
  if (vkBindBufferMemory(device, out.buffer, out.memory, 0) != VK_SUCCESS) {
    reason = "vkBindBufferMemory failed";
    return false;
  }
  return true;
}

}  // namespace

bool Device::ReadbackRgba8(const uint8_t* pixels, uint32_t width, uint32_t height,
                           std::vector<uint8_t>& out, std::string& reason) {
  if (vulkan_ == nullptr || vulkan_->device == VK_NULL_HANDLE) {
    reason = "device is not open";
    return false;
  }
  if (pixels == nullptr || width == 0 || height == 0) {
    reason = "empty image";
    return false;
  }
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4u;
  Buffer source{};
  Buffer readback{};
  Image image{};
  VkCommandBuffer command = VK_NULL_HANDLE;
  bool ok = false;
  auto cleanup = [&]() {
    if (command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(vulkan_->device, vulkan_->pool, 1, &command);
    }
    DestroyImage(vulkan_->device, image);
    DestroyBuffer(vulkan_->device, readback);
    DestroyBuffer(vulkan_->device, source);
  };

  if (!MakeBuffer(vulkan_->physical, vulkan_->device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, source,
                  reason) ||
      !MakeBuffer(vulkan_->physical, vulkan_->device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  readback, reason)) {
    cleanup();
    return false;
  }
  void* mapped = nullptr;
  if (vkMapMemory(vulkan_->device, source.memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
    reason = "vkMapMemory failed";
    cleanup();
    return false;
  }
  std::memcpy(mapped, pixels, size_t(bytes));
  vkUnmapMemory(vulkan_->device, source.memory);

  VkImageCreateInfo image_info{};
  image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(vulkan_->device, &image_info, nullptr, &image.image) != VK_SUCCESS) {
    reason = "vkCreateImage failed";
    cleanup();
    return false;
  }
  VkMemoryRequirements image_requirements{};
  vkGetImageMemoryRequirements(vulkan_->device, image.image, &image_requirements);
  uint32_t image_type =
      FindMemory(vulkan_->physical, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (image_type == UINT32_MAX) {
    image_type = FindMemory(vulkan_->physical, image_requirements.memoryTypeBits, 0);
  }
  if (image_type == UINT32_MAX) {
    reason = "no memory for an image";
    cleanup();
    return false;
  }
  VkMemoryAllocateInfo image_allocate{};
  image_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  image_allocate.allocationSize = image_requirements.size;
  image_allocate.memoryTypeIndex = image_type;
  if (vkAllocateMemory(vulkan_->device, &image_allocate, nullptr, &image.memory) != VK_SUCCESS ||
      vkBindImageMemory(vulkan_->device, image.image, image.memory, 0) != VK_SUCCESS) {
    reason = "image memory allocation failed";
    cleanup();
    return false;
  }

  VkCommandBufferAllocateInfo allocate_command{};
  allocate_command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_command.commandPool = vulkan_->pool;
  allocate_command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_command.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(vulkan_->device, &allocate_command, &command) != VK_SUCCESS) {
    reason = "vkAllocateCommandBuffers failed";
    cleanup();
    return false;
  }
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    reason = "vkBeginCommandBuffer failed";
    cleanup();
    return false;
  }

  VkImageMemoryBarrier to_destination{};
  to_destination.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_destination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_destination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_destination.image = image.image;
  to_destination.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  to_destination.subresourceRange.levelCount = 1;
  to_destination.subresourceRange.layerCount = 1;
  to_destination.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_destination.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_destination.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &to_destination);

  VkBufferImageCopy copy_in{};
  copy_in.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy_in.imageSubresource.layerCount = 1;
  copy_in.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(command, source.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &copy_in);

  VkImageMemoryBarrier to_source = to_destination;
  to_source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  to_source.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &to_source);
  vkCmdCopyImageToBuffer(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                         1, &copy_in);

  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    reason = "vkEndCommandBuffer failed";
    cleanup();
    return false;
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  if (vkQueueSubmit(vulkan_->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
      vkQueueWaitIdle(vulkan_->queue) != VK_SUCCESS) {
    reason = "transfer submit failed";
    cleanup();
    return false;
  }

  void* read = nullptr;
  if (vkMapMemory(vulkan_->device, readback.memory, 0, bytes, 0, &read) != VK_SUCCESS) {
    reason = "readback map failed";
    cleanup();
    return false;
  }
  out.resize(size_t(bytes));
  std::memcpy(out.data(), read, size_t(bytes));
  vkUnmapMemory(vulkan_->device, readback.memory);
  ok = true;
  cleanup();
  return ok;
}

namespace {

bool MapBlendFactor(uint32_t factor, VkBlendFactor& out) {
  switch (factor) {
    case 0:
      out = VK_BLEND_FACTOR_ZERO;
      return true;
    case 1:
      out = VK_BLEND_FACTOR_ONE;
      return true;
    case 4:
      out = VK_BLEND_FACTOR_SRC_COLOR;
      return true;
    case 5:
      out = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
      return true;
    case 6:
      out = VK_BLEND_FACTOR_SRC_ALPHA;
      return true;
    case 7:
      out = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      return true;
    case 8:
      out = VK_BLEND_FACTOR_DST_COLOR;
      return true;
    case 9:
      out = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
      return true;
    case 10:
      out = VK_BLEND_FACTOR_DST_ALPHA;
      return true;
    case 11:
      out = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
      return true;
    case 12:
      out = VK_BLEND_FACTOR_CONSTANT_COLOR;
      return true;
    case 13:
      out = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
      return true;
    case 14:
      out = VK_BLEND_FACTOR_CONSTANT_ALPHA;
      return true;
    case 15:
      out = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
      return true;
    case 16:
      out = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
      return true;
    default:
      return false;
  }
}

bool MapBlendOp(uint32_t op, VkBlendOp& out) {
  switch (op) {
    case 0:
      out = VK_BLEND_OP_ADD;
      return true;
    case 1:
      out = VK_BLEND_OP_SUBTRACT;
      return true;
    case 2:
      out = VK_BLEND_OP_MIN;
      return true;
    case 3:
      out = VK_BLEND_OP_MAX;
      return true;
    case 4:
      out = VK_BLEND_OP_REVERSE_SUBTRACT;
      return true;
    default:
      return false;
  }
}

struct DecodedRaster {
  bool depth = false;
  bool depth_write = false;
  VkCompareOp depth_op = VK_COMPARE_OP_ALWAYS;
  VkCullModeFlags cull = VK_CULL_MODE_NONE;
  VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  bool blend = false;
  VkBlendFactor src_color = VK_BLEND_FACTOR_ONE;
  VkBlendFactor dst_color = VK_BLEND_FACTOR_ZERO;
  VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE;
  VkBlendFactor dst_alpha = VK_BLEND_FACTOR_ZERO;
  VkBlendOp color_op = VK_BLEND_OP_ADD;
  VkBlendOp alpha_op = VK_BLEND_OP_ADD;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

bool DecodeRaster(const Device::RasterState& raster, uint32_t width, DecodedRaster& out,
                  std::string& reason, bool multisample = false) {
  rex::graphics::reg::RB_SURFACE_INFO surface{};
  surface.value = raster.surface_info;
  if (surface.surface_pitch != width) {
    reason = "surface pitch does not match the draw width";
    return false;
  }
  switch (static_cast<uint32_t>(surface.msaa_samples)) {
    case 0:
      out.samples = VK_SAMPLE_COUNT_1_BIT;
      break;
    case 1:
      out.samples = VK_SAMPLE_COUNT_2_BIT;
      break;
    case 2:
      out.samples = VK_SAMPLE_COUNT_4_BIT;
      break;
    default:
      reason = "surface MSAA is not 1x, 2x, or 4x";
      return false;
  }
  if (!multisample && out.samples != VK_SAMPLE_COUNT_1_BIT) {
    reason = "surface MSAA is not 1x";
    return false;
  }
  rex::graphics::reg::PA_SU_SC_MODE_CNTL mode{};
  mode.value = raster.mode_cntl;
  if (mode.cull_front != 0 && mode.cull_back != 0) {
    out.cull = VK_CULL_MODE_FRONT_AND_BACK;
  } else if (mode.cull_front != 0) {
    out.cull = VK_CULL_MODE_FRONT_BIT;
  } else if (mode.cull_back != 0) {
    out.cull = VK_CULL_MODE_BACK_BIT;
  }
  out.front = mode.face != 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;

  rex::graphics::reg::RB_DEPTHCONTROL depth{};
  depth.value = raster.depth_control;
  out.depth = depth.z_enable != 0;
  out.depth_write = depth.z_write_enable != 0;
  const uint32_t compare = static_cast<uint32_t>(depth.zfunc);
  if (compare > 7) {
    reason = "depth compare is out of range";
    return false;
  }
  out.depth_op = static_cast<VkCompareOp>(compare);

  rex::graphics::reg::RB_BLENDCONTROL blend{};
  blend.value = raster.blend_control;
  out.blend = true;
  if (!MapBlendFactor(static_cast<uint32_t>(blend.color_srcblend), out.src_color) ||
      !MapBlendFactor(static_cast<uint32_t>(blend.color_destblend), out.dst_color) ||
      !MapBlendFactor(static_cast<uint32_t>(blend.alpha_srcblend), out.src_alpha) ||
      !MapBlendFactor(static_cast<uint32_t>(blend.alpha_destblend), out.dst_alpha) ||
      !MapBlendOp(static_cast<uint32_t>(blend.color_comb_fcn), out.color_op) ||
      !MapBlendOp(static_cast<uint32_t>(blend.alpha_comb_fcn), out.alpha_op)) {
    reason = "blend factor is not a Xenos factor";
    return false;
  }
  return true;
}

}  // namespace

bool Device::DrawTexturedRgba8(const uint8_t* pixels, uint32_t width, uint32_t height,
                               std::vector<uint8_t>& out, std::string& reason,
                               const RasterState* fixed_state) {
  if (vulkan_ == nullptr || vulkan_->device == VK_NULL_HANDLE) {
    reason = "device is not open";
    return false;
  }
  if (!info_.dynamic_rendering) {
    reason = "dynamic rendering is not enabled";
    return false;
  }
  if (pixels == nullptr || width == 0 || height == 0) {
    reason = "empty image";
    return false;
  }
  DecodedRaster decoded;
  if (fixed_state != nullptr && !DecodeRaster(*fixed_state, width, decoded, reason, true)) {
    return false;
  }

  // Fullscreen triangle. The vertex stage writes a vec2 at location 0, and the
  // fragment stage reads that same location to choose the texel.
  static constexpr uint32_t kVertex[] = {
      0x07230203, 0x00010500, 0x0008000b, 0x0000002b, 0x00000000, 0x00020011, 0x00000001,
      0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
      0x00000000, 0x00000001, 0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000,
      0x00000009, 0x0000000c, 0x0000001d, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
      0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x00007675, 0x00060005,
      0x0000000c, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00060005, 0x0000001b,
      0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000001b, 0x00000000,
      0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001b, 0x00000001, 0x505f6c67,
      0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000001b, 0x00000002, 0x435f6c67,
      0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x0000001b, 0x00000003, 0x435f6c67,
      0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x0000001d, 0x00000000, 0x00040047,
      0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000c, 0x0000000b, 0x0000002a,
      0x00030047, 0x0000001b, 0x00000002, 0x00050048, 0x0000001b, 0x00000000, 0x0000000b,
      0x00000000, 0x00050048, 0x0000001b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048,
      0x0000001b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001b, 0x00000003,
      0x0000000b, 0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
      0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002,
      0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
      0x00000003, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020, 0x0000000b,
      0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b,
      0x0000000a, 0x0000000e, 0x00000001, 0x0004002b, 0x0000000a, 0x00000010, 0x00000002,
      0x00040017, 0x00000017, 0x00000006, 0x00000004, 0x00040015, 0x00000018, 0x00000020,
      0x00000000, 0x0004002b, 0x00000018, 0x00000019, 0x00000001, 0x0004001c, 0x0000001a,
      0x00000006, 0x00000019, 0x0006001e, 0x0000001b, 0x00000017, 0x00000006, 0x0000001a,
      0x0000001a, 0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b, 0x0000001c,
      0x0000001d, 0x00000003, 0x0004002b, 0x0000000a, 0x0000001e, 0x00000000, 0x0004002b,
      0x00000006, 0x00000020, 0x40000000, 0x0004002b, 0x00000006, 0x00000022, 0x3f800000,
      0x0004002b, 0x00000006, 0x00000025, 0x00000000, 0x00040020, 0x00000029, 0x00000003,
      0x00000017, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
      0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4, 0x0000000a,
      0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a, 0x00000011, 0x0000000f,
      0x00000010, 0x0004006f, 0x00000006, 0x00000012, 0x00000011, 0x0004003d, 0x0000000a,
      0x00000013, 0x0000000c, 0x000500c7, 0x0000000a, 0x00000014, 0x00000013, 0x00000010,
      0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016,
      0x00000012, 0x00000015, 0x0003003e, 0x00000009, 0x00000016, 0x0004003d, 0x00000007,
      0x0000001f, 0x00000009, 0x0005008e, 0x00000007, 0x00000021, 0x0000001f, 0x00000020,
      0x00050050, 0x00000007, 0x00000023, 0x00000022, 0x00000022, 0x00050083, 0x00000007,
      0x00000024, 0x00000021, 0x00000023, 0x00050051, 0x00000006, 0x00000026, 0x00000024,
      0x00000000, 0x00050051, 0x00000006, 0x00000027, 0x00000024, 0x00000001, 0x00070050,
      0x00000017, 0x00000028, 0x00000026, 0x00000027, 0x00000025, 0x00000022, 0x00050041,
      0x00000029, 0x0000002a, 0x0000001d, 0x0000001e, 0x0003003e, 0x0000002a, 0x00000028,
      0x000100fd, 0x00010038};
  static constexpr uint32_t kFragment[] = {
      0x07230203, 0x00010500, 0x0008000b, 0x00000023, 0x00000000, 0x00020011, 0x00000001,
      0x00020011, 0x00000032, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
      0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000004, 0x00000004,
      0x6e69616d, 0x00000000, 0x0000000e, 0x00000016, 0x0000001e, 0x00030010, 0x00000004,
      0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
      0x00000000, 0x00040005, 0x00000009, 0x657a6973, 0x00000000, 0x00030005, 0x0000000e,
      0x00786574, 0x00040005, 0x00000013, 0x726f6f63, 0x00000064, 0x00030005, 0x00000016,
      0x00007675, 0x00040005, 0x0000001e, 0x6f6c6f63, 0x00000072, 0x00040047, 0x0000000e,
      0x00000021, 0x00000000, 0x00040047, 0x0000000e, 0x00000022, 0x00000000, 0x00040047,
      0x00000016, 0x0000001e, 0x00000000, 0x00040047, 0x0000001e, 0x0000001e, 0x00000000,
      0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
      0x00000020, 0x00000001, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020,
      0x00000008, 0x00000007, 0x00000007, 0x00030016, 0x0000000a, 0x00000020, 0x00090019,
      0x0000000b, 0x0000000a, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
      0x00000000, 0x0003001b, 0x0000000c, 0x0000000b, 0x00040020, 0x0000000d, 0x00000000,
      0x0000000c, 0x0004003b, 0x0000000d, 0x0000000e, 0x00000000, 0x0004002b, 0x00000006,
      0x00000010, 0x00000000, 0x00040017, 0x00000014, 0x0000000a, 0x00000002, 0x00040020,
      0x00000015, 0x00000001, 0x00000014, 0x0004003b, 0x00000015, 0x00000016, 0x00000001,
      0x00040017, 0x0000001c, 0x0000000a, 0x00000004, 0x00040020, 0x0000001d, 0x00000003,
      0x0000001c, 0x0004003b, 0x0000001d, 0x0000001e, 0x00000003, 0x00050036, 0x00000002,
      0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008,
      0x00000009, 0x00000007, 0x0004003b, 0x00000008, 0x00000013, 0x00000007, 0x0004003d,
      0x0000000c, 0x0000000f, 0x0000000e, 0x00040064, 0x0000000b, 0x00000011, 0x0000000f,
      0x00050067, 0x00000007, 0x00000012, 0x00000011, 0x00000010, 0x0003003e, 0x00000009,
      0x00000012, 0x0004003d, 0x00000014, 0x00000017, 0x00000016, 0x0004003d, 0x00000007,
      0x00000018, 0x00000009, 0x0004006f, 0x00000014, 0x00000019, 0x00000018, 0x00050085,
      0x00000014, 0x0000001a, 0x00000017, 0x00000019, 0x0004006e, 0x00000007, 0x0000001b,
      0x0000001a, 0x0003003e, 0x00000013, 0x0000001b, 0x0004003d, 0x0000000c, 0x0000001f,
      0x0000000e, 0x0004003d, 0x00000007, 0x00000020, 0x00000013, 0x00040064, 0x0000000b,
      0x00000021, 0x0000001f, 0x0007005f, 0x0000001c, 0x00000022, 0x00000021, 0x00000020,
      0x00000002, 0x00000010, 0x0003003e, 0x0000001e, 0x00000022, 0x000100fd, 0x00010038};

  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4u;
  const VkDevice device = vulkan_->device;
  Buffer source{};
  Buffer readback{};
  Image texture{};
  Image target{};
  Image resolve{};
  Image depth{};
  VkImageView texture_view = VK_NULL_HANDLE;
  VkImageView target_view = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkShaderModule vertex_module = VK_NULL_HANDLE;
  VkShaderModule fragment_module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;

  auto cleanup = [&]() {
    if (command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device, vulkan_->pool, 1, &command);
    }
    if (pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (vertex_module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(device, vertex_module, nullptr);
    }
    if (fragment_module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(device, fragment_module, nullptr);
    }
    if (descriptor_pool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    }
    if (pipeline_layout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    }
    if (set_layout != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    }
    if (sampler != VK_NULL_HANDLE) {
      vkDestroySampler(device, sampler, nullptr);
    }
    if (texture_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, texture_view, nullptr);
    }
    if (target_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, target_view, nullptr);
    }
    if (depth_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, depth_view, nullptr);
    }
    DestroyImage(device, depth);
    DestroyImage(device, resolve);
    DestroyImage(device, target);
    DestroyImage(device, texture);
    DestroyBuffer(device, readback);
    DestroyBuffer(device, source);
  };

  auto make_image = [&](VkImageUsageFlags usage, Image& image, VkSampleCountFlagBits samples) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &info, nullptr, &image.image) != VK_SUCCESS) {
      reason = "vkCreateImage failed";
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image.image, &requirements);
    uint32_t type = FindMemory(vulkan_->physical, requirements.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) {
      type = FindMemory(vulkan_->physical, requirements.memoryTypeBits, 0);
    }
    if (type == UINT32_MAX) {
      reason = "no memory for an image";
      return false;
    }
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, image.image, image.memory, 0) != VK_SUCCESS) {
      reason = "image memory allocation failed";
      return false;
    }
    return true;
  };

  if (!MakeBuffer(vulkan_->physical, device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, source,
                  reason) ||
      !MakeBuffer(vulkan_->physical, device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                  readback, reason) ||
      !make_image(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, texture,
                  VK_SAMPLE_COUNT_1_BIT) ||
      !make_image(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, target,
                  decoded.samples) ||
      (decoded.samples != VK_SAMPLE_COUNT_1_BIT &&
       !make_image(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, resolve,
                   VK_SAMPLE_COUNT_1_BIT))) {
    cleanup();
    return false;
  }
  if (decoded.depth) {
    VkImageCreateInfo depth_info{};
    depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = VK_FORMAT_D32_SFLOAT;
    depth_info.extent = {width, height, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = decoded.samples;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemoryRequirements depth_requirements{};
    uint32_t depth_type = UINT32_MAX;
    if (vkCreateImage(device, &depth_info, nullptr, &depth.image) != VK_SUCCESS) {
      reason = "depth image was rejected";
      cleanup();
      return false;
    }
    vkGetImageMemoryRequirements(device, depth.image, &depth_requirements);
    depth_type = FindMemory(vulkan_->physical, depth_requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (depth_type == UINT32_MAX) {
      depth_type = FindMemory(vulkan_->physical, depth_requirements.memoryTypeBits, 0);
    }
    VkMemoryAllocateInfo depth_allocate{};
    depth_allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depth_allocate.allocationSize = depth_requirements.size;
    depth_allocate.memoryTypeIndex = depth_type;
    if (depth_type == UINT32_MAX ||
        vkAllocateMemory(device, &depth_allocate, nullptr, &depth.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, depth.image, depth.memory, 0) != VK_SUCCESS) {
      reason = "depth memory allocation failed";
      cleanup();
      return false;
    }
    VkImageViewCreateInfo depth_view_info{};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = depth.image;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = VK_FORMAT_D32_SFLOAT;
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_info.subresourceRange.levelCount = 1;
    depth_view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &depth_view_info, nullptr, &depth_view) != VK_SUCCESS) {
      reason = "depth view was rejected";
      cleanup();
      return false;
    }
  }
  void* mapped = nullptr;
  if (vkMapMemory(device, source.memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
    reason = "vkMapMemory failed";
    cleanup();
    return false;
  }
  std::memcpy(mapped, pixels, size_t(bytes));
  vkUnmapMemory(device, source.memory);

  auto make_view = [&](VkImage image, VkImageView& view) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    return vkCreateImageView(device, &info, nullptr, &view) == VK_SUCCESS;
  };
  if (!make_view(texture.image, texture_view) || !make_view(target.image, target_view)) {
    reason = "vkCreateImageView failed";
    cleanup();
    return false;
  }

  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_NEAREST;
  sampler_info.minFilter = VK_FILTER_NEAREST;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
    reason = "vkCreateSampler failed";
    cleanup();
    return false;
  }

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{};
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout) != VK_SUCCESS) {
    reason = "descriptor set layout was rejected";
    cleanup();
    return false;
  }
  VkPipelineLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
    reason = "pipeline layout was rejected";
    cleanup();
    return false;
  }

  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VkDescriptorSetAllocateInfo allocate_set{};
  allocate_set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocate_set.descriptorSetCount = 1;
  allocate_set.pSetLayouts = &set_layout;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
    reason = "vkCreateDescriptorPool failed";
    cleanup();
    return false;
  }
  allocate_set.descriptorPool = descriptor_pool;
  if (vkAllocateDescriptorSets(device, &allocate_set, &descriptor_set) != VK_SUCCESS) {
    reason = "vkAllocateDescriptorSets failed";
    cleanup();
    return false;
  }
  VkDescriptorImageInfo image_info{};
  image_info.sampler = sampler;
  image_info.imageView = texture_view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = descriptor_set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &image_info;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

  VkShaderModuleCreateInfo module_info{};
  module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  module_info.codeSize = sizeof(kVertex);
  module_info.pCode = kVertex;
  if (vkCreateShaderModule(device, &module_info, nullptr, &vertex_module) != VK_SUCCESS) {
    reason = "draw vertex shader was rejected";
    cleanup();
    return false;
  }
  module_info.codeSize = sizeof(kFragment);
  module_info.pCode = kFragment;
  if (vkCreateShaderModule(device, &module_info, nullptr, &fragment_module) != VK_SUCCESS) {
    reason = "draw fragment shader was rejected";
    cleanup();
    return false;
  }

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
  VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, {width, height}};
  VkPipelineViewportStateCreateInfo viewport_state{};
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = &viewport;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = fixed_state == nullptr ? VK_CULL_MODE_NONE : decoded.cull;
  raster.frontFace = fixed_state == nullptr ? VK_FRONT_FACE_COUNTER_CLOCKWISE : decoded.front;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = decoded.samples;
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (fixed_state != nullptr) {
    attachment.blendEnable = decoded.blend ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = decoded.src_color;
    attachment.dstColorBlendFactor = decoded.dst_color;
    attachment.colorBlendOp = decoded.color_op;
    attachment.srcAlphaBlendFactor = decoded.src_alpha;
    attachment.dstAlphaBlendFactor = decoded.dst_alpha;
    attachment.alphaBlendOp = decoded.alpha_op;
  }
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  VkPipelineDepthStencilStateCreateInfo depth_state{};
  depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depth_state.depthTestEnable = decoded.depth ? VK_TRUE : VK_FALSE;
  depth_state.depthWriteEnable = decoded.depth_write ? VK_TRUE : VK_FALSE;
  depth_state.depthCompareOp = decoded.depth_op;
  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
  VkPipelineRenderingCreateInfo rendering_format{};
  rendering_format.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering_format.colorAttachmentCount = 1;
  rendering_format.pColorAttachmentFormats = &color_format;
  if (decoded.depth) {
    rendering_format.depthAttachmentFormat = depth_format;
  }
  VkGraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_info.pNext = &rendering_format;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &input_assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = decoded.depth ? &depth_state : nullptr;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.layout = pipeline_layout;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    reason = "draw pipeline was rejected";
    cleanup();
    return false;
  }

  VkCommandBufferAllocateInfo allocate_command{};
  allocate_command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_command.commandPool = vulkan_->pool;
  allocate_command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_command.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device, &allocate_command, &command) != VK_SUCCESS) {
    reason = "vkAllocateCommandBuffers failed";
    cleanup();
    return false;
  }
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
    reason = "vkBeginCommandBuffer failed";
    cleanup();
    return false;
  }

  VkImageMemoryBarrier barriers[2]{};
  for (VkImageMemoryBarrier& barrier : barriers) {
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  }
  barriers[0].image = texture.image;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barriers[1].image = target.image;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barriers[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       0, 0, nullptr, 0, nullptr, 2, barriers);
  if (decoded.depth) {
    VkImageMemoryBarrier depth_barrier{};
    depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.image = depth.image;
    depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_barrier.subresourceRange.levelCount = 1;
    depth_barrier.subresourceRange.layerCount = 1;
    depth_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &depth_barrier);
  }

  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(command, source.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &copy);

  VkImageMemoryBarrier to_sample = barriers[0];
  to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &to_sample);

  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = target_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  if (fixed_state != nullptr) {
    color.clearValue.color.float32[0] = fixed_state->clear_red;
    color.clearValue.color.float32[1] = fixed_state->clear_green;
    color.clearValue.color.float32[2] = fixed_state->clear_blue;
    color.clearValue.color.float32[3] = fixed_state->clear_alpha;
  }
  VkRenderingAttachmentInfo depth_attachment{};
  depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depth_attachment.imageView = depth_view;
  depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.clearValue.depthStencil.depth = 1.f;
  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea = {{0, 0}, {width, height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  if (decoded.depth) {
    rendering.pDepthAttachment = &depth_attachment;
  }
  vkCmdBeginRendering(command, &rendering);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                          &descriptor_set, 0, nullptr);
  vkCmdDraw(command, 3, 1, 0, 0);
  vkCmdEndRendering(command);

  VkImageMemoryBarrier to_copy = barriers[1];
  to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  to_copy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  to_copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_copy);
  VkImage copy_source = target.image;
  if (decoded.samples != VK_SAMPLE_COUNT_1_BIT) {
    VkImageMemoryBarrier to_resolve{};
    to_resolve.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_resolve.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_resolve.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_resolve.image = resolve.image;
    to_resolve.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_resolve.subresourceRange.levelCount = 1;
    to_resolve.subresourceRange.layerCount = 1;
    to_resolve.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_resolve.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_resolve.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_resolve);
    VkImageResolve region{};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent = {width, height, 1};
    vkCmdResolveImage(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolve.image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    to_resolve.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_resolve.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_resolve.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_resolve.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_resolve);
    copy_source = resolve.image;
  }
  vkCmdCopyImageToBuffer(command, copy_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
                         1, &copy);
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    reason = "vkEndCommandBuffer failed";
    cleanup();
    return false;
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  if (vkQueueSubmit(vulkan_->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
      vkQueueWaitIdle(vulkan_->queue) != VK_SUCCESS) {
    reason = "draw submit failed";
    cleanup();
    return false;
  }

  void* read = nullptr;
  if (vkMapMemory(device, readback.memory, 0, bytes, 0, &read) != VK_SUCCESS) {
    reason = "readback map failed";
    cleanup();
    return false;
  }
  out.resize(size_t(bytes));
  std::memcpy(out.data(), read, size_t(bytes));
  vkUnmapMemory(device, readback.memory);
  cleanup();
  return true;
}

bool Device::DrawGuestFragment(const uint32_t* words, size_t word_count, uint32_t width,
                               uint32_t height, uint32_t image_count, uint32_t sampler_count,
                               uint32_t texture_key, bool write_interpolator, float interpolator_x,
                               bool guest_vertex, const VertexStream* streams, uint32_t stream_count,
                               std::vector<uint8_t>& out, std::string& reason,
                               const uint32_t* fragment_words, size_t fragment_word_count,
                               const uint8_t* texture_rgba, uint32_t texture_width,
                               uint32_t texture_height, const RasterState* fixed_state) {
  if (vulkan_ == nullptr || words == nullptr || word_count < 5 || width == 0 || height == 0 ||
      image_count > 16 || sampler_count > 8 || !info_.dynamic_rendering) {
    reason = "guest draw is missing a device, a shader, or dynamic rendering";
    return false;
  }
  DecodedRaster decoded;
  if (fixed_state != nullptr && !DecodeRaster(*fixed_state, width, decoded, reason)) {
    return false;
  }
  constexpr uint32_t kPosition[] = {
      0x07230203, 0x00010500, 0x0008000b, 0x0000002b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
      0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
      0x0007000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x0000001d, 0x00030003,
      0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009,
      0x00007675, 0x00060005, 0x0000000c, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00060005,
      0x0000001b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000001b, 0x00000000,
      0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001b, 0x00000001, 0x505f6c67, 0x746e696f,
      0x657a6953, 0x00000000, 0x00070006, 0x0000001b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
      0x0065636e, 0x00070006, 0x0000001b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
      0x00030005, 0x0000001d, 0x00000000, 0x00040047, 0x0000000c, 0x0000000b, 0x0000002a, 0x00030047,
      0x0000001b, 0x00000002, 0x00050048, 0x0000001b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048,
      0x0000001b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000001b, 0x00000002, 0x0000000b,
      0x00000003, 0x00050048, 0x0000001b, 0x00000003, 0x0000000b, 0x00000004, 0x00020013, 0x00000002,
      0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
      0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040015, 0x0000000a,
      0x00000020, 0x00000001, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b,
      0x0000000c, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000e, 0x00000001, 0x0004002b, 0x0000000a,
      0x00000010, 0x00000002, 0x00040017, 0x00000017, 0x00000006, 0x00000004, 0x00040015, 0x00000018,
      0x00000020, 0x00000000, 0x0004002b, 0x00000018, 0x00000019, 0x00000001, 0x0004001c, 0x0000001a,
      0x00000006, 0x00000019, 0x0006001e, 0x0000001b, 0x00000017, 0x00000006, 0x0000001a, 0x0000001a,
      0x00040020, 0x0000001c, 0x00000003, 0x0000001b, 0x0004003b, 0x0000001c, 0x0000001d, 0x00000003,
      0x0004002b, 0x0000000a, 0x0000001e, 0x00000000, 0x0004002b, 0x00000006, 0x00000020, 0x40000000,
      0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x0004002b, 0x00000006, 0x00000025, 0x00000000,
      0x00040020, 0x00000029, 0x00000003, 0x00000017, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
      0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003d,
      0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4, 0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e,
      0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010, 0x0004006f, 0x00000006, 0x00000012,
      0x00000011, 0x0004003d, 0x0000000a, 0x00000013, 0x0000000c, 0x000500c7, 0x0000000a, 0x00000014,
      0x00000013, 0x00000010, 0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007,
      0x00000016, 0x00000012, 0x00000015, 0x0003003e, 0x00000009, 0x00000016, 0x0004003d, 0x00000007,
      0x0000001f, 0x00000009, 0x0005008e, 0x00000007, 0x00000021, 0x0000001f, 0x00000020, 0x00050050,
      0x00000007, 0x00000023, 0x00000022, 0x00000022, 0x00050083, 0x00000007, 0x00000024, 0x00000021,
      0x00000023, 0x00050051, 0x00000006, 0x00000026, 0x00000024, 0x00000000, 0x00050051, 0x00000006,
      0x00000027, 0x00000024, 0x00000001, 0x00070050, 0x00000017, 0x00000028, 0x00000026, 0x00000027,
      0x00000025, 0x00000022, 0x00050041, 0x00000029, 0x0000002a, 0x0000001d, 0x0000001e, 0x0003003e,
      0x0000002a, 0x00000028, 0x000100fd, 0x00010038};
  VkDevice device = vulkan_->device;
  VkShaderModule vertex_module = VK_NULL_HANDLE;
  VkShaderModule fragment_module = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layouts[4] = {};
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet sets[4] = {};
  Buffer shared{};
  Buffer constants[4]{};
  Buffer vertex_floats{};
  Image target{};
  Image depth{};
  VkImageView target_view = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  Image texture{};
  VkImageView texture_view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  Buffer texture_upload{};
  Buffer readback{};
  VkCommandBuffer command = VK_NULL_HANDLE;
  constexpr uint32_t kTextureSize = 32;
  const bool custom_texture = texture_rgba != nullptr;
  const uint32_t tex_w = custom_texture ? texture_width : kTextureSize;
  const uint32_t tex_h = custom_texture ? texture_height : kTextureSize;
  if (image_count != 0 && (tex_w == 0 || tex_h == 0 || (tex_w & 31u) != 0)) {
    reason = "guest texture width is not a multiple of 32";
    return false;
  }
  const auto cleanup = [&]() {
    if (command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device, vulkan_->pool, 1, &command);
    }
    DestroyBuffer(device, readback);
    DestroyBuffer(device, texture_upload);
    if (sampler != VK_NULL_HANDLE) {
      vkDestroySampler(device, sampler, nullptr);
    }
    if (texture_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, texture_view, nullptr);
    }
    DestroyImage(device, texture);
    if (target_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, target_view, nullptr);
    }
    if (depth_view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, depth_view, nullptr);
    }
    DestroyImage(device, depth);
    DestroyImage(device, target);
    for (Buffer& constant : constants) {
      DestroyBuffer(device, constant);
    }
    DestroyBuffer(device, vertex_floats);
    DestroyBuffer(device, shared);
    if (pipeline != VK_NULL_HANDLE) {
      vkDestroyPipeline(device, pipeline, nullptr);
    }
    if (layout != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device, pool, nullptr);
    }
    for (VkDescriptorSetLayout set_layout : set_layouts) {
      if (set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
      }
    }
    if (fragment_module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(device, fragment_module, nullptr);
    }
    if (vertex_module != VK_NULL_HANDLE) {
      vkDestroyShaderModule(device, vertex_module, nullptr);
    }
  };

  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  if (guest_vertex) {
    module_info.codeSize = word_count * sizeof(uint32_t);
    module_info.pCode = words;
  } else if (write_interpolator) {
    module_info.codeSize = kInterpolatorVertexWords * sizeof(uint32_t);
    module_info.pCode = kInterpolatorVertex;
  } else {
    module_info.codeSize = sizeof(kPosition);
    module_info.pCode = kPosition;
  }
  if (vkCreateShaderModule(device, &module_info, nullptr, &vertex_module) != VK_SUCCESS) {
    reason = "guest position shader failed";
    cleanup();
    return false;
  }
  if (guest_vertex && fragment_words != nullptr && fragment_word_count != 0) {
    module_info.codeSize = fragment_word_count * sizeof(uint32_t);
    module_info.pCode = fragment_words;
  } else if (guest_vertex) {
    module_info.codeSize = kPassFragmentWords * sizeof(uint32_t);
    module_info.pCode = kPassFragment;
  } else {
    module_info.codeSize = word_count * sizeof(uint32_t);
    module_info.pCode = words;
  }
  if (vkCreateShaderModule(device, &module_info, nullptr, &fragment_module) != VK_SUCCESS) {
    reason = "guest fragment shader failed";
    cleanup();
    return false;
  }
  const VkShaderStageFlags guest_stages =
      guest_vertex ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                   : VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutBinding storage{};
  storage.binding = 0;
  storage.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  storage.descriptorCount = 1;
  storage.stageFlags = guest_stages;
  VkDescriptorSetLayoutBinding uniforms[5]{};
  const uint32_t uniform_bindings[4] = {0, 2, 3, 4};
  uint32_t uniform_count = 4;
  for (uint32_t i = 0; i < 4; ++i) {
    uniforms[i].binding = uniform_bindings[i];
    uniforms[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniforms[i].descriptorCount = 1;
    uniforms[i].stageFlags = guest_stages;
  }
  if (guest_vertex) {
    uniforms[4].binding = 1;
    uniforms[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniforms[4].descriptorCount = 1;
    uniforms[4].stageFlags = guest_stages;
    uniform_count = 5;
  }
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &storage;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layouts[0]) != VK_SUCCESS) {
    reason = "guest storage layout failed";
    cleanup();
    return false;
  }
  set_info.bindingCount = uniform_count;
  set_info.pBindings = uniforms;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layouts[1]) != VK_SUCCESS) {
    reason = "guest constant layout failed";
    cleanup();
    return false;
  }
  set_info.bindingCount = 0;
  set_info.pBindings = nullptr;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layouts[2]) != VK_SUCCESS) {
    reason = "guest vertex texture layout failed";
    cleanup();
    return false;
  }
  std::vector<VkDescriptorSetLayoutBinding> texture_bindings(image_count + sampler_count);
  for (uint32_t i = 0; i < image_count; ++i) {
    texture_bindings[i].binding = i;
    texture_bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    texture_bindings[i].descriptorCount = 1;
    texture_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  for (uint32_t i = 0; i < sampler_count; ++i) {
    texture_bindings[image_count + i].binding = image_count + i;
    texture_bindings[image_count + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    texture_bindings[image_count + i].descriptorCount = 1;
    texture_bindings[image_count + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  set_info.bindingCount = uint32_t(texture_bindings.size());
  set_info.pBindings = texture_bindings.empty() ? nullptr : texture_bindings.data();
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layouts[3]) != VK_SUCCESS) {
    reason = "guest texture layout failed";
    cleanup();
    return false;
  }
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 4;
  layout_info.pSetLayouts = set_layouts;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
    reason = "guest pipeline layout failed";
    cleanup();
    return false;
  }

  constexpr VkDeviceSize kBytes = 65536;
  constexpr VkMemoryPropertyFlags kHost =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  if (!MakeBuffer(vulkan_->physical, device, kBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, kHost,
                  shared, reason) ||
      !MakeBuffer(vulkan_->physical, device, kBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost,
                  constants[0], reason) ||
      !MakeBuffer(vulkan_->physical, device, kBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost,
                  constants[1], reason) ||
      !MakeBuffer(vulkan_->physical, device, kBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost,
                  constants[2], reason) ||
      !MakeBuffer(vulkan_->physical, device, kBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost,
                  constants[3], reason)) {
    cleanup();
    return false;
  }
  for (Buffer* buffer : {&shared, &constants[0], &constants[1], &constants[2], &constants[3]}) {
    void* mapped = nullptr;
    if (vkMapMemory(device, buffer->memory, 0, kBytes, 0, &mapped) != VK_SUCCESS) {
      reason = "guest constant map failed";
      cleanup();
      return false;
    }
    std::memset(mapped, 0, size_t(kBytes));
    if (buffer == &constants[0]) {
      // Alpha test function 7 is "always". color_exp_bias stays at 1 so the
      // shader's own color is what gets stored. Swizzle RGBA for every fetch.
      auto* system = static_cast<uint8_t*>(mapped);
      // Alpha test function 7 is "always". Bit 10 keeps the vertex shader's W
      // instead of treating it as 1/W. The host NDC scale is 1 so a clip-space
      // triangle is not multiplied by zero. vertex_index_max (byte 500) must
      // be above the vertex ids or every fetch index is clamped to 0.
      const uint32_t alpha_test_always = (7u << 16) | (1u << 10);
      std::memcpy(system, &alpha_test_always, sizeof(alpha_test_always));
      const float ndc_scale[3] = {1.f, 1.f, 1.f};
      std::memcpy(system + 124, ndc_scale, sizeof(ndc_scale));
      const uint32_t vertex_index_max = 0x00FFFFFFu;
      std::memcpy(system + 500, &vertex_index_max, sizeof(vertex_index_max));
      const uint32_t swizzle = 0x688u | (0x688u << 12);
      for (uint32_t i = 0; i < 16; ++i) {
        std::memcpy(system + 208 + i * sizeof(uint32_t), &swizzle, sizeof(swizzle));
      }
      const float color_scale[4] = {1.f, 1.f, 1.f, 1.f};
      std::memcpy(system + 288, color_scale, sizeof(color_scale));
    } else if (buffer == &constants[1]) {
      auto* values = static_cast<float*>(mapped);
      const float pattern[4] = {1.f, 0.5f, 0.25f, 1.f};
      for (uint32_t i = 0; i < 256 * 4; ++i) {
        values[i] = pattern[i & 3];
      }
    } else if (buffer == &constants[3] && !guest_vertex) {
      rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
      fetch.type = rex::graphics::xenos::FetchConstantType::kTexture;
      fetch.clamp_x = rex::graphics::xenos::ClampMode::kClampToEdge;
      fetch.clamp_y = rex::graphics::xenos::ClampMode::kClampToEdge;
      fetch.clamp_z = rex::graphics::xenos::ClampMode::kClampToEdge;
      fetch.pitch = tex_w >> 5;
      fetch.format = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
      fetch.size_2d.width = tex_w - 1;
      fetch.size_2d.height = tex_h - 1;
      fetch.swizzle = rex::graphics::xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA;
      fetch.mag_filter = rex::graphics::xenos::TextureFilter::kPoint;
      fetch.min_filter = rex::graphics::xenos::TextureFilter::kPoint;
      fetch.mip_filter = rex::graphics::xenos::TextureFilter::kBaseMap;
      fetch.dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
      auto* words_out = static_cast<rex::graphics::xenos::xe_gpu_texture_fetch_t*>(mapped);
      for (uint32_t i = 0; i < 32; ++i) {
        words_out[i] = fetch;
      }
    }
    vkUnmapMemory(device, buffer->memory);
  }
  if (guest_vertex) {
    if (!MakeBuffer(vulkan_->physical, device, kBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost,
                    vertex_floats, reason)) {
      cleanup();
      return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device, vertex_floats.memory, 0, kBytes, 0, &mapped) != VK_SUCCESS) {
      reason = "guest vertex constant map failed";
      cleanup();
      return false;
    }
    auto* values = static_cast<float*>(mapped);
    for (uint32_t reg = 0; reg < 256; ++reg) {
      values[reg * 4 + 0] = (reg % 4) == 0 ? 1.f : 0.f;
      values[reg * 4 + 1] = (reg % 4) == 1 ? 1.f : 0.f;
      values[reg * 4 + 2] = (reg % 4) == 2 ? 1.f : 0.f;
      values[reg * 4 + 3] = (reg % 4) == 3 ? 1.f : 0.f;
    }
    vkUnmapMemory(device, vertex_floats.memory);
  }
  uint32_t guest_vertices = 3;
  if (guest_vertex && streams != nullptr && stream_count != 0) {
    void* memory_mapped = nullptr;
    void* fetch_mapped = nullptr;
    if (vkMapMemory(device, shared.memory, 0, kBytes, 0, &memory_mapped) != VK_SUCCESS ||
        vkMapMemory(device, constants[3].memory, 0, kBytes, 0, &fetch_mapped) != VK_SUCCESS) {
      reason = "guest vertex memory map failed";
      if (memory_mapped != nullptr) {
        vkUnmapMemory(device, shared.memory);
      }
      cleanup();
      return false;
    }
    auto* memory = static_cast<uint32_t*>(memory_mapped);
    auto* fetch = static_cast<uint32_t*>(fetch_mapped);
    constexpr uint32_t kDwords = 65536 / 4;
    const float triangle[3][4] = {
        {-1.f, -1.f, 0.f, 1.f},
        {3.f, -1.f, 0.f, 1.f},
        {-1.f, 3.f, 0.f, 1.f},
    };
    uint32_t cursor = 64;
    for (uint32_t s = 0; s < stream_count; ++s) {
      const VertexStream& stream = streams[s];
      if (stream.fetch_constant >= 96) {
        continue;
      }
      uint32_t stride = stream.stride_words == 0 ? 4u : stream.stride_words;
      if (cursor + stride * 3 > kDwords) {
        break;
      }
      const uint32_t base = cursor;
      fetch[stream.fetch_constant * 2] = (base << 2) | 3u;
      fetch[stream.fetch_constant * 2 + 1] = 0xFFFFFFu << 2;
      if (stream.bytes != nullptr) {
        const uint32_t stride_bytes = stride * 4u;
        if (stride_bytes == 0 || stream.byte_count < stride_bytes * 3u ||
            stream.byte_count % stride_bytes != 0 || cursor + stream.byte_count / 4u > kDwords) {
          reason = "vertex stream does not fit the draw";
          vkUnmapMemory(device, constants[3].memory);
          vkUnmapMemory(device, shared.memory);
          cleanup();
          return false;
        }
        std::memcpy(memory + base, stream.bytes, stream.byte_count);
        guest_vertices = stream.byte_count / stride_bytes;
        cursor += stream.byte_count / 4u;
        continue;
      }
      for (uint32_t vertex = 0; vertex < 3; ++vertex) {
        for (uint32_t a = 0; a < stream.attribute_count && a < 8; ++a) {
          const VertexAttribute& attribute = stream.attributes[a];
          const int32_t at = stream.stride_words == 0
                                 ? int32_t(base) + attribute.offset_words
                                 : int32_t(base + vertex * stream.stride_words) + attribute.offset_words;
          if (attribute.short4) {
            const int32_t scale = stream.short_scale == 0 ? 1 : stream.short_scale;
            const int32_t w_scale = stream.short_w_scale == 0 ? scale : stream.short_w_scale;
            const bool covered = a == stream.triangle_attribute;
            const int16_t packed[4] = {
                static_cast<int16_t>(covered ? triangle[vertex][0] * float(scale) : 0.f),
                static_cast<int16_t>(covered ? triangle[vertex][1] * float(scale) : 0.f),
                static_cast<int16_t>(covered ? triangle[vertex][2] * float(scale) : 0.f),
                static_cast<int16_t>(covered ? w_scale : 1),
            };
            if (at < 0 || uint32_t(at) + 2 > kDwords) {
              continue;
            }
            memory[at] = (uint32_t(uint16_t(packed[1])) << 16) | uint16_t(packed[0]);
            memory[at + 1] = (uint32_t(uint16_t(packed[3])) << 16) | uint16_t(packed[2]);
            continue;
          }
          if (!attribute.is_float || attribute.components == 0 ||
              (a != stream.triangle_attribute && !attribute.constant_fill)) {
            continue;
          }
          if (at < 0 || uint32_t(at) + attribute.components > kDwords) {
            continue;
          }
          const float constant_value[4] = {1.f, 0.5f, 0.25f, 1.f};
          for (uint32_t c = 0; c < attribute.components && c < 4; ++c) {
            const float value =
                a == stream.triangle_attribute ? triangle[vertex][c] : constant_value[c];
            std::memcpy(memory + at + c, &value, sizeof(float));
          }
        }
      }
      cursor += stride * 3;
    }
    vkUnmapMemory(device, constants[3].memory);
    vkUnmapMemory(device, shared.memory);
  }

  if (image_count != 0) {
    std::vector<uint8_t> pixels(size_t(tex_w) * tex_h * 4);
    if (custom_texture) {
      std::memcpy(pixels.data(), texture_rgba, pixels.size());
    } else {
      for (uint32_t y = 0; y < tex_h; ++y) {
        for (uint32_t x = 0; x < tex_w; ++x) {
          uint8_t* pixel = pixels.data() + (size_t(y) * tex_w + x) * 4;
          if (texture_key == 0) {
            pixel[0] = uint8_t(17 + x);
            pixel[1] = uint8_t(34 + y);
            pixel[2] = 51;
            pixel[3] = 255;
          } else {
            pixel[0] = 200;
            pixel[1] = 10;
            pixel[2] = 20;
            pixel[3] = 255;
          }
        }
      }
    }
    if (!MakeBuffer(vulkan_->physical, device, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    kHost, texture_upload, reason)) {
      cleanup();
      return false;
    }
    void* upload = nullptr;
    if (vkMapMemory(device, texture_upload.memory, 0, pixels.size(), 0, &upload) != VK_SUCCESS) {
      reason = "guest texture upload failed";
      cleanup();
      return false;
    }
    std::memcpy(upload, pixels.data(), pixels.size());
    vkUnmapMemory(device, texture_upload.memory);

    VkImageCreateInfo texture_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    texture_info.imageType = VK_IMAGE_TYPE_2D;
    texture_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    texture_info.extent = {tex_w, tex_h, 1};
    texture_info.mipLevels = 1;
    texture_info.arrayLayers = 1;
    texture_info.samples = VK_SAMPLE_COUNT_1_BIT;
    texture_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    texture_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    texture_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &texture_info, nullptr, &texture.image) != VK_SUCCESS) {
      reason = "guest texture image failed";
      cleanup();
      return false;
    }
    VkMemoryRequirements texture_requirements{};
    vkGetImageMemoryRequirements(device, texture.image, &texture_requirements);
    const uint32_t texture_memory = FindMemory(vulkan_->physical, texture_requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (texture_memory == UINT32_MAX) {
      reason = "guest texture memory failed";
      cleanup();
      return false;
    }
    VkMemoryAllocateInfo texture_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    texture_allocate.allocationSize = texture_requirements.size;
    texture_allocate.memoryTypeIndex = texture_memory;
    if (vkAllocateMemory(device, &texture_allocate, nullptr, &texture.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, texture.image, texture.memory, 0) != VK_SUCCESS) {
      reason = "guest texture bind failed";
      cleanup();
      return false;
    }
    VkImageViewCreateInfo texture_view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    texture_view_info.image = texture.image;
    texture_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    texture_view_info.format = texture_info.format;
    texture_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    texture_view_info.subresourceRange.levelCount = 1;
    texture_view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &texture_view_info, nullptr, &texture_view) != VK_SUCCESS) {
      reason = "guest texture view failed";
      cleanup();
      return false;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.f;
    if (vkCreateSampler(device, &sampler_info, nullptr, &sampler) != VK_SUCCESS) {
      reason = "guest sampler failed";
      cleanup();
      return false;
    }
  }

  VkDescriptorPoolSize pool_sizes[4]{};
  pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_sizes[0].descriptorCount = 1;
  pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  pool_sizes[1].descriptorCount = uniform_count;
  uint32_t pool_count = 2;
  if (image_count != 0) {
    pool_sizes[pool_count].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[pool_count].descriptorCount = image_count;
    ++pool_count;
  }
  if (sampler_count != 0) {
    pool_sizes[pool_count].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[pool_count].descriptorCount = sampler_count;
    ++pool_count;
  }
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 4;
  pool_info.poolSizeCount = pool_count;
  pool_info.pPoolSizes = pool_sizes;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &pool) != VK_SUCCESS) {
    reason = "guest descriptor pool failed";
    cleanup();
    return false;
  }
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = pool;
  allocate.descriptorSetCount = 4;
  allocate.pSetLayouts = set_layouts;
  if (vkAllocateDescriptorSets(device, &allocate, sets) != VK_SUCCESS) {
    reason = "guest descriptor allocation failed";
    cleanup();
    return false;
  }
  VkDescriptorBufferInfo buffer_infos[6]{};
  Buffer* bound[5] = {&shared, &constants[0], &constants[1], &constants[2], &constants[3]};
  for (uint32_t i = 0; i < 5; ++i) {
    buffer_infos[i].buffer = bound[i]->buffer;
    buffer_infos[i].range = kBytes;
  }
  VkWriteDescriptorSet writes[6]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet = sets[0];
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo = &buffer_infos[0];
  for (uint32_t i = 0; i < 4; ++i) {
    writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i + 1].dstSet = sets[1];
    writes[i + 1].dstBinding = uniform_bindings[i];
    writes[i + 1].descriptorCount = 1;
    writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[i + 1].pBufferInfo = &buffer_infos[i + 1];
  }
  uint32_t write_count = 5;
  if (guest_vertex) {
    buffer_infos[5].buffer = vertex_floats.buffer;
    buffer_infos[5].range = kBytes;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = sets[1];
    writes[5].dstBinding = 1;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[5].pBufferInfo = &buffer_infos[5];
    write_count = 6;
  }
  std::vector<VkDescriptorImageInfo> image_infos(image_count + sampler_count);
  std::vector<VkWriteDescriptorSet> texture_writes(image_count + sampler_count);
  for (uint32_t i = 0; i < image_count; ++i) {
    image_infos[i].imageView = texture_view;
    image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    texture_writes[i].dstSet = sets[3];
    texture_writes[i].dstBinding = i;
    texture_writes[i].descriptorCount = 1;
    texture_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    texture_writes[i].pImageInfo = &image_infos[i];
  }
  for (uint32_t i = 0; i < sampler_count; ++i) {
    image_infos[image_count + i].sampler = sampler;
    texture_writes[image_count + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    texture_writes[image_count + i].dstSet = sets[3];
    texture_writes[image_count + i].dstBinding = image_count + i;
    texture_writes[image_count + i].descriptorCount = 1;
    texture_writes[image_count + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    texture_writes[image_count + i].pImageInfo = &image_infos[image_count + i];
  }
  vkUpdateDescriptorSets(device, write_count, writes, 0, nullptr);
  if (!texture_writes.empty()) {
    vkUpdateDescriptorSets(device, uint32_t(texture_writes.size()), texture_writes.data(), 0,
                           nullptr);
  }

  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_R8G8B8A8_UNORM;
  image.extent = {width, height, 1};
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &image, nullptr, &target.image) != VK_SUCCESS) {
    reason = "guest target failed";
    cleanup();
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, target.image, &requirements);
  const uint32_t memory_type =
      FindMemory(vulkan_->physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memory_type == UINT32_MAX) {
    reason = "guest target memory failed";
    cleanup();
    return false;
  }
  VkMemoryAllocateInfo allocate_image{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_image.allocationSize = requirements.size;
  allocate_image.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device, &allocate_image, nullptr, &target.memory) != VK_SUCCESS ||
      vkBindImageMemory(device, target.image, target.memory, 0) != VK_SUCCESS) {
    reason = "guest target bind failed";
    cleanup();
    return false;
  }
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = target.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = image.format;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = 1;
  view.subresourceRange.layerCount = 1;
  if (vkCreateImageView(device, &view, nullptr, &target_view) != VK_SUCCESS) {
    reason = "guest target view failed";
    cleanup();
    return false;
  }
  if (decoded.depth) {
    VkImageCreateInfo depth_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = VK_FORMAT_D32_SFLOAT;
    depth_info.extent = {width, height, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &depth_info, nullptr, &depth.image) != VK_SUCCESS) {
      reason = "guest depth image failed";
      cleanup();
      return false;
    }
    VkMemoryRequirements depth_requirements{};
    vkGetImageMemoryRequirements(device, depth.image, &depth_requirements);
    const uint32_t depth_type = FindMemory(vulkan_->physical, depth_requirements.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo depth_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    depth_allocate.allocationSize = depth_requirements.size;
    depth_allocate.memoryTypeIndex = depth_type;
    if (depth_type == UINT32_MAX ||
        vkAllocateMemory(device, &depth_allocate, nullptr, &depth.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, depth.image, depth.memory, 0) != VK_SUCCESS) {
      reason = "guest depth memory failed";
      cleanup();
      return false;
    }
    VkImageViewCreateInfo depth_view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    depth_view_info.image = depth.image;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = VK_FORMAT_D32_SFLOAT;
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_info.subresourceRange.levelCount = 1;
    depth_view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &depth_view_info, nullptr, &depth_view) != VK_SUCCESS) {
      reason = "guest depth view failed";
      cleanup();
      return false;
    }
  }
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
  if (!MakeBuffer(vulkan_->physical, device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHost, readback,
                  reason)) {
    cleanup();
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertex_module;
  stages[0].pName = "main";
  VkSpecializationMapEntry spec_entry{};
  spec_entry.constantID = 0;
  spec_entry.size = sizeof(float);
  VkSpecializationInfo spec{};
  spec.mapEntryCount = 1;
  spec.pMapEntries = &spec_entry;
  spec.dataSize = sizeof(float);
  spec.pData = &interpolator_x;
  if (write_interpolator && !guest_vertex) {
    stages[0].pSpecializationInfo = &spec;
  }
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragment_module;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{};
  viewport.width = float(width);
  viewport.height = float(height);
  viewport.maxDepth = 1.f;
  VkRect2D scissor{{0, 0}, {width, height}};
  VkPipelineViewportStateCreateInfo viewport_state{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = &viewport;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = fixed_state == nullptr ? VK_CULL_MODE_NONE : decoded.cull;
  raster.frontFace = fixed_state == nullptr ? VK_FRONT_FACE_COUNTER_CLOCKWISE : decoded.front;
  raster.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = 0xF;
  if (fixed_state != nullptr) {
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = decoded.src_color;
    blend.dstColorBlendFactor = decoded.dst_color;
    blend.colorBlendOp = decoded.color_op;
    blend.srcAlphaBlendFactor = decoded.src_alpha;
    blend.dstAlphaBlendFactor = decoded.dst_alpha;
    blend.alphaBlendOp = decoded.alpha_op;
  }
  VkPipelineColorBlendStateCreateInfo blend_state{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend_state.attachmentCount = 1;
  blend_state.pAttachments = &blend;
  VkPipelineDepthStencilStateCreateInfo depth_state{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth_state.depthTestEnable = decoded.depth ? VK_TRUE : VK_FALSE;
  depth_state.depthWriteEnable = decoded.depth_write ? VK_TRUE : VK_FALSE;
  depth_state.depthCompareOp = decoded.depth_op;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
  rendering.pColorAttachmentFormats = &color_format;
  if (decoded.depth) {
    rendering.depthAttachmentFormat = depth_format;
  }
  VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.pNext = &rendering;
  pipeline_info.stageCount = 2;
  pipeline_info.pStages = stages;
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &assembly;
  pipeline_info.pViewportState = &viewport_state;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &multisample;
  pipeline_info.pDepthStencilState = decoded.depth ? &depth_state : nullptr;
  pipeline_info.pColorBlendState = &blend_state;
  pipeline_info.layout = layout;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    reason = "guest pipeline failed";
    cleanup();
    return false;
  }

  VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  command_info.commandPool = vulkan_->pool;
  command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_info.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device, &command_info, &command) != VK_SUCCESS) {
    reason = "guest command buffer failed";
    cleanup();
    return false;
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(command, &begin);
  if (texture_view != VK_NULL_HANDLE) {
    VkImageMemoryBarrier upload_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    upload_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    upload_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    upload_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    upload_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    upload_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    upload_barrier.image = texture.image;
    upload_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    upload_barrier.subresourceRange.levelCount = 1;
    upload_barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &upload_barrier);
    VkBufferImageCopy upload_copy{};
    upload_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    upload_copy.imageSubresource.layerCount = 1;
    upload_copy.imageExtent = {tex_w, tex_h, 1};
    vkCmdCopyBufferToImage(command, texture_upload.buffer, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_copy);
    upload_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    upload_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    upload_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    upload_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &upload_barrier);
  }
  VkImageMemoryBarrier target_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  target_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  target_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  target_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  target_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  target_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  target_barrier.image = target.image;
  target_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  target_barrier.subresourceRange.levelCount = 1;
  target_barrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &target_barrier);
  if (decoded.depth) {
    VkImageMemoryBarrier depth_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth_barrier.image = depth.image;
    depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_barrier.subresourceRange.levelCount = 1;
    depth_barrier.subresourceRange.layerCount = 1;
    depth_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &depth_barrier);
  }
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = target_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color.float32[0] = fixed_state == nullptr ? 1.f : fixed_state->clear_red;
  color.clearValue.color.float32[1] = fixed_state == nullptr ? 0.f : fixed_state->clear_green;
  color.clearValue.color.float32[2] = fixed_state == nullptr ? 1.f : fixed_state->clear_blue;
  color.clearValue.color.float32[3] = fixed_state == nullptr ? 1.f : fixed_state->clear_alpha;
  VkRenderingAttachmentInfo depth_attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth_attachment.imageView = depth_view;
  depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_attachment.clearValue.depthStencil.depth = 1.f;
  VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
  render.renderArea.extent = {width, height};
  render.layerCount = 1;
  render.colorAttachmentCount = 1;
  render.pColorAttachments = &color;
  if (decoded.depth) {
    render.pDepthAttachment = &depth_attachment;
  }
  vkCmdBeginRendering(command, &render);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 4, sets, 0, nullptr);
  vkCmdDraw(command, guest_vertices, 1, 0, 0);
  vkCmdEndRendering(command);
  target_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  target_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  target_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  target_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &target_barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy.imageSubresource.layerCount = 1;
  copy.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         readback.buffer, 1, &copy);
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    reason = "guest command recording failed";
    cleanup();
    return false;
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  if (vkQueueSubmit(vulkan_->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
      vkQueueWaitIdle(vulkan_->queue) != VK_SUCCESS) {
    reason = "guest draw submit failed";
    cleanup();
    return false;
  }
  void* read = nullptr;
  if (vkMapMemory(device, readback.memory, 0, bytes, 0, &read) != VK_SUCCESS) {
    reason = "guest readback map failed";
    cleanup();
    return false;
  }
  out.resize(size_t(bytes));
  std::memcpy(out.data(), read, size_t(bytes));
  vkUnmapMemory(device, readback.memory);
  cleanup();
  return true;
}

}  // namespace broken_bond::gfx
