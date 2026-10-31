// Host memory for the browser build. Replaces memory_posix.cpp / memory_win.cpp.
//
// WebAssembly has one linear memory: the guest window is reserved with sbrk
// and the address views fold onto it (guest_fold.h). Page protection is
// bookkeeping; this file does not unmap the window.

#include "web/guest_fold.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include <unistd.h>

#include <rex/assert.h>
#include <rex/math.h>
#include <rex/memory/utils.h>

namespace rex::memory {
namespace {

constexpr uint64_t kWindowTail = 0x10000;
constexpr uint64_t kWindowAlignment = 0x10000;

std::mutex host_allocations_mutex;
std::map<uintptr_t, size_t> host_allocations;
uint8_t* guest_window = nullptr;

bool InGuestWindow(const void* pointer, size_t length) {
  const auto address = reinterpret_cast<uintptr_t>(pointer);
  const auto begin = reinterpret_cast<uintptr_t>(guest_window);
  return guest_window && address >= begin &&
         length <= broken_bond::web::kGuestWindowSize + kWindowTail &&
         address - begin <= broken_bond::web::kGuestWindowSize + kWindowTail - length;
}

bool FindHostAllocation(const void* pointer, size_t length, uintptr_t* base_out, size_t* length_out) {
  const auto address = reinterpret_cast<uintptr_t>(pointer);
  std::lock_guard<std::mutex> lock(host_allocations_mutex);
  auto it = host_allocations.upper_bound(address);
  if (it == host_allocations.begin()) return false;
  --it;
  if (address - it->first > it->second || length > it->second - (address - it->first)) return false;
  if (base_out) *base_out = it->first;
  if (length_out) *length_out = it->second;
  return true;
}

}  // namespace

uint8_t* AllocateGuestWindow() {
  static std::once_flag once;
  std::call_once(once, [] {
    const auto current = reinterpret_cast<uintptr_t>(sbrk(0));
    const auto padding = rex::round_up(current, uintptr_t(kWindowAlignment)) - current;
    uint64_t remaining = padding + broken_bond::web::kGuestWindowSize + kWindowTail;
    uint8_t* start = nullptr;
    uint8_t* expected = nullptr;
    while (remaining) {
      const uint64_t step = std::min<uint64_t>(remaining, uint64_t(1) << 30);
      void* result = sbrk(intptr_t(step));
      if (result == reinterpret_cast<void*>(-1) || (expected && result != expected)) {
        std::fprintf(stderr, "web: cannot reserve the guest window\n");
        return;
      }
      start = start ? start : static_cast<uint8_t*>(result);
      expected = static_cast<uint8_t*>(result) + step;
      remaining -= step;
    }
    guest_window = start + padding;
  });
  return guest_window;
}

size_t page_size() { return 4096; }

size_t allocation_granularity() { return 4096; }

bool IsWritableExecutableMemorySupported() { return false; }

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type, PageAccess access) {
  (void)allocation_type;
  (void)access;
  if (!base_address) {
    const size_t rounded = rex::round_up(length, page_size());
    void* result = std::aligned_alloc(page_size(), rounded);
    if (!result) return nullptr;
    std::memset(result, 0, rounded);
    std::lock_guard<std::mutex> lock(host_allocations_mutex);
    host_allocations[reinterpret_cast<uintptr_t>(result)] = rounded;
    return result;
  }
  if (InGuestWindow(base_address, length) || FindHostAllocation(base_address, length, nullptr, nullptr)) {
    return base_address;
  }
  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  if (InGuestWindow(base_address, length)) return true;
  uintptr_t allocation_base = 0;
  size_t allocation_length = 0;
  if (!FindHostAllocation(base_address, length, &allocation_base, &allocation_length)) return false;
  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      std::memset(base_address, 0, length);
      return true;
    case DeallocationType::kRelease:
      if (allocation_base != reinterpret_cast<uintptr_t>(base_address)) return false;
      {
        std::lock_guard<std::mutex> lock(host_allocations_mutex);
        host_allocations.erase(allocation_base);
      }
      std::free(base_address);
      return true;
    default:
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  (void)access;
  if (out_old_access) *out_old_access = PageAccess::kReadWrite;
  return InGuestWindow(base_address, length) || FindHostAllocation(base_address, length, nullptr, nullptr);
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;
  length = 0;
  const auto address = reinterpret_cast<uintptr_t>(base_address);
  if (InGuestWindow(base_address, 1)) {
    length = reinterpret_cast<uintptr_t>(guest_window) + broken_bond::web::kGuestWindowSize + kWindowTail - address;
    access_out = PageAccess::kReadWrite;
    return true;
  }
  uintptr_t allocation_base = 0;
  size_t allocation_length = 0;
  if (FindHostAllocation(base_address, 1, &allocation_base, &allocation_length)) {
    length = allocation_base + allocation_length - address;
    access_out = PageAccess::kReadWrite;
    return true;
  }
  return false;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path&, size_t, PageAccess, bool) {
  return kFileMappingHandleInvalid;
}

void CloseFileMappingHandle(FileMappingHandle, const std::filesystem::path&) {}

void* MapFileView(FileMappingHandle, void*, size_t, PageAccess, size_t) { return nullptr; }

bool UnmapFileView(FileMappingHandle, void*, size_t) { return false; }

}  // namespace rex::memory
