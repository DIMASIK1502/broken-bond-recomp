// Xbox Vision Camera is not required for Naruto: The Broken Bond gameplay.
// These exports represent an unavailable camera: Create succeeds so callers
// that treat it as subsystem init keep going, GetState reports disconnected,
// and capture/config/read APIs return DEVICE_NOT_CONNECTED without fabricating
// frames or a fake device handle.

#include <atomic>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

void LogUnavailableOnce(std::atomic<bool>& logged, const char* name, u32 result) {
  bool expected = false;
  if (logged.compare_exchange_strong(expected, true)) {
    REXKRNL_WARN("{}: Xbox Vision Camera unavailable; returning {:#x}", name, result);
  }
}

// XUsbcamCreate is treated as usbcam-subsystem init, not "a camera exists".
// Xenia returns success because some titles skip remaining init on failure.
// Broken Bond's wrapper continues either way, but success plus a null handle
// is the documented unavailable-device state.
//
// r3 = guest capture buffer (already allocated by the caller, 0x4B000)
// r4 = buffer size
// r5 = PHANDLE (image global at +15088)
u32 XUsbcamCreate_entry(u32 buffer, u32 buffer_size, mapped_u32 handle_out) {
  (void)buffer;
  (void)buffer_size;
  if (handle_out) {
    *handle_out = 0;
  }
  static std::atomic<bool> logged{false};
  LogUnavailableOnce(logged, "XUsbcamCreate", X_STATUS_SUCCESS);
  return X_STATUS_SUCCESS;
}

// Destroy's return is ignored; the caller then stores 0 into the handle global.
u32 XUsbcamDestroy_entry(u32 handle) {
  (void)handle;
  static std::atomic<bool> logged{false};
  LogUnavailableOnce(logged, "XUsbcamDestroy", X_STATUS_SUCCESS);
  return X_STATUS_SUCCESS;
}

// sub_825F68E0 only enters capture setup if the state equals 2.
// 0 = disconnected.
u32 XUsbcamGetState_entry() {
  static std::atomic<bool> logged{false};
  LogUnavailableOnce(logged, "XUsbcamGetState", 0);
  return 0;
}

// Callers treat r3==0 as success (SetConfig continues applying entries;
// SetCaptureMode arms capture and later ReadFrame). Do not return 0.
// Results are Win32 DWORD (callers compare against 997 = ERROR_IO_PENDING).
u32 XUsbcamSetConfig_entry(u32 handle) {
  (void)handle;
  static std::atomic<bool> logged{false};
  LogUnavailableOnce(logged, "XUsbcamSetConfig", X_ERROR_DEVICE_NOT_CONNECTED);
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

u32 XUsbcamSetView_entry(u32 handle) {
  (void)handle;
  static std::atomic<bool> logged{false};
  LogUnavailableOnce(logged, "XUsbcamSetView", X_ERROR_DEVICE_NOT_CONNECTED);
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

u32 XUsbcamSetCaptureMode_entry(u32 handle) {
  (void)handle;
  static std::atomic<bool> logged{false};
  LogUnavailableOnce(logged, "XUsbcamSetCaptureMode", X_ERROR_DEVICE_NOT_CONNECTED);
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

u32 XUsbcamReadFrame_entry(u32 handle) {
  (void)handle;
  static std::atomic<bool> logged{false};
  LogUnavailableOnce(logged, "XUsbcamReadFrame", X_ERROR_DEVICE_NOT_CONNECTED);
  return X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace

REX_EXPORT(__imp__XUsbcamCreate, XUsbcamCreate_entry)
REX_EXPORT(__imp__XUsbcamDestroy, XUsbcamDestroy_entry)
REX_EXPORT(__imp__XUsbcamGetState, XUsbcamGetState_entry)
REX_EXPORT(__imp__XUsbcamSetConfig, XUsbcamSetConfig_entry)
REX_EXPORT(__imp__XUsbcamSetView, XUsbcamSetView_entry)
REX_EXPORT(__imp__XUsbcamSetCaptureMode, XUsbcamSetCaptureMode_entry)
REX_EXPORT(__imp__XUsbcamReadFrame, XUsbcamReadFrame_entry)
