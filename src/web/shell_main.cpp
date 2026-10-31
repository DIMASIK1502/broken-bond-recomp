#include "broken_bond_init.h"
#include "web/web_audio.h"
#include "web/web_files.h"
#include "web/web_input.h"

#include <filesystem>
#include <iostream>

#include <emscripten.h>

#include <rex/audio/nop/nop_audio_system.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/kernel/init.h>
#include <rex/logging.h>
#include <rex/runtime.h>
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
  config.audio_factory = REX_AUDIO_BACKEND(rex::audio::nop::NopAudioSystem);
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
