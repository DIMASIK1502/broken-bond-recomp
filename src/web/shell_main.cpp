#include "broken_bond_init.h"
#include "web/web_audio.h"
#include "web/web_files.h"
#include "web/web_input.h"

#include <filesystem>
#include <iostream>

#include <emscripten.h>

#include <rex/audio/audio_driver.h>
#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xthread.h>

#include <imgui.h>

using rex::X_STATUS;

// The guest thread keeps running after it starts, and its program counter is
// not stored. The link register is the last call, which is enough to see
// whether boot is waiting on the GPU, a kernel service, or the title.
extern "C" EMSCRIPTEN_KEEPALIVE void broken_bond_web_where() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->kernel_state()) {
    std::cout << "broken_bond web: no kernel" << std::endl;
    return;
  }
  auto* kernel = runtime->kernel_state();
  for (uint32_t id = 1; id <= 32; ++id) {
    auto thread = kernel->GetThreadByID(id);
    if (!thread || !thread->thread_state()) continue;
    auto* ctx = thread->thread_state()->context();
    const auto* params = thread->creation_params();
    std::cout << "broken_bond web: thread " << std::dec << id
              << (thread->main_thread() ? " main" : "")
              << (thread->is_running() ? " running" : " idle")
              << " start " << std::hex << (params ? params->start_address : 0)
              << " lr " << (ctx ? ctx->lr : 0) << " ctr " << (ctx ? ctx->ctr.u32 : 0)
              << std::endl;
  }
}

// The only function that publishes the D3D device, and the destructor that
// clears it. Both are reached through the function table.
REX_EXTERN(sub_821C1190);
REX_EXTERN(sub_821C2260);
REX_EXTERN(__imp__sub_821C20A8);
REX_EXTERN(__imp__sub_821C2188);
REX_EXTERN(__imp__sub_821C28A0);
REX_EXTERN(__imp__sub_821C2C80);
REX_EXTERN(__imp__sub_821C1998);
REX_EXTERN(__imp__sub_821CBB60);

static void WebLogGuest(const char* name, PPCContext& ctx, uint8_t* base, void (*impl)(PPCContext&, uint8_t*)) {
  impl(ctx, base);
  std::cout << "broken_bond web: " << name << " -> " << std::hex << ctx.r3.u32 << std::dec << std::endl;
}

extern "C" void sub_821C20A8(PPCContext& ctx, uint8_t* base) {
  WebLogGuest("step 20A8", ctx, base, __imp__sub_821C20A8);
}
extern "C" void sub_821C2188(PPCContext& ctx, uint8_t* base) {
  WebLogGuest("ctor 2188", ctx, base, __imp__sub_821C2188);
}
extern "C" void sub_821C28A0(PPCContext& ctx, uint8_t* base) {
  WebLogGuest("init 28A0", ctx, base, __imp__sub_821C28A0);
}
extern "C" void sub_821C2C80(PPCContext& ctx, uint8_t* base) {
  WebLogGuest("step 2C80", ctx, base, __imp__sub_821C2C80);
}
extern "C" void sub_821C1998(PPCContext& ctx, uint8_t* base) {
  WebLogGuest("step 1998", ctx, base, __imp__sub_821C1998);
}
extern "C" void sub_821CBB60(PPCContext& ctx, uint8_t* base) {
  WebLogGuest("step BB60", ctx, base, __imp__sub_821CBB60);
}

// The nop backend refuses every client, and the guest treats that status as a
// failed device. This driver accepts the client and drops the frames.
class SilentAudioDriver final : public rex::audio::AudioDriver {
 public:
  using AudioDriver::AudioDriver;
  void SubmitFrame(uint32_t) override {}
};

class SilentAudioSystem final : public rex::audio::nop::NopAudioSystem {
 public:
  using NopAudioSystem::NopAudioSystem;

  static std::unique_ptr<rex::audio::AudioSystem> Create(rex::runtime::FunctionDispatcher* dispatcher) {
    return std::make_unique<SilentAudioSystem>(dispatcher);
  }

  rex::X_STATUS CreateDriver(size_t, rex::thread::Semaphore*, rex::audio::AudioDriver** out_driver) override {
    *out_driver = new SilentAudioDriver(memory());
    return X_STATUS_SUCCESS;
  }

  void DestroyDriver(rex::audio::AudioDriver* driver) override { delete driver; }
};

static uint32_t WebDeviceSlot() {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return 0;
  return rex::memory::load_and_swap<uint32_t>(runtime->memory()->TranslateVirtual(0x833A37E0));
}

static void WebDeviceSetup(PPCContext& ctx, uint8_t* base) {
  std::cout << "broken_bond web: device setup lr " << std::hex << static_cast<uint32_t>(ctx.lr)
            << " r3 " << ctx.r3.u32 << " r4 " << ctx.r4.u32 << " slot " << WebDeviceSlot() << std::dec
            << std::endl;
  sub_821C1190(ctx, base);
  std::cout << "broken_bond web: device setup result " << std::hex << ctx.r3.u32 << " slot "
            << WebDeviceSlot() << std::dec << std::endl;
}

static void WebDeviceShutdown(PPCContext& ctx, uint8_t* base) {
  std::cout << "broken_bond web: device shutdown lr " << std::hex << static_cast<uint32_t>(ctx.lr)
            << " object " << ctx.r3.u32 << " slot " << WebDeviceSlot() << std::dec << std::endl;
  sub_821C2260(ctx, base);
  std::cout << "broken_bond web: device shutdown done slot " << std::hex << WebDeviceSlot()
            << std::dec << std::endl;
}

// Boots the recompiled guest in this page and presents on #canvas.

int main(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();
  ImGui::CreateContext();
  broken_bond::web::InputBlock().visible = 1;
  (void)broken_bond::web::AudioRing();
  if (!broken_bond::web::InitializeStorage()) {
    return 1;
  }

  const std::filesystem::path game = REXCVAR_GET(game_data_root);
  const std::filesystem::path user = game.parent_path() / "user";
  const std::filesystem::path cache = game.parent_path() / "cache";
  std::error_code ec;
  std::filesystem::create_directories(user, ec);
  std::filesystem::create_directories(cache, ec);

  std::cout << "broken_bond web: game " << game.string() << "\n";
  rex::Runtime runtime(game, user, {}, cache);
  rex::RuntimeConfig config;
  config.gpu_plugin = "host";
  config.graphics = rex::system::LoadGpuPlugin("host", "any");
  config.audio_factory = REX_AUDIO_BACKEND(SilentAudioSystem);
  config.input_factory = REX_INPUT_BACKEND(rex::input::CreateDefaultInputSystem);
  config.kernel_init = rex::kernel::InitializeKernel;

  X_STATUS status = runtime.Setup(PPCImageConfig, std::move(config));
  if (status != X_STATUS_SUCCESS) {
    std::cout << "broken_bond web: runtime setup failed " << std::hex << status << "\n";
    return 1;
  }
  if (PPCImageConfig.register_modules) {
    PPCImageConfig.register_modules(runtime.kernel_state());
  }

  status = runtime.LoadXexImage("game:\\default.xex");
  if (status != X_STATUS_SUCCESS) {
    std::cout << "broken_bond web: failed to load default.xex " << std::hex << status << "\n";
    return 1;
  }

  if (auto* dispatcher = runtime.function_dispatcher()) {
    dispatcher->SetFunction(0x821C1190, WebDeviceSetup);
    dispatcher->SetFunction(0x821C2260, WebDeviceShutdown);
  }

  auto main_thread = runtime.PrepareModuleLaunch();
  if (!main_thread) {
    std::cout << "broken_bond web: no guest thread\n";
    return 1;
  }
  main_thread->Resume();
  std::cout << "broken_bond web: guest thread started\n";
  main_thread->Wait(0, 0, 0, nullptr);
  std::cout << "broken_bond web: guest thread returned\n";
  return 0;
}
