// Title-side runtime hooks: cutscene hitch mitigation and save-location logging.

#include "generated/default/broken_bond_init.h"
#include "broken_bond_app.h"

#include <filesystem>
#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace {

void LogExistingSaves(const std::filesystem::path& user_data_root) {
  std::error_code ec;
  if (!std::filesystem::exists(user_data_root, ec)) {
    REXLOG_INFO("No user-data directory yet; first save will create {}",
                user_data_root.string());
    return;
  }

  size_t found = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(user_data_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory()) {
      continue;
    }
    const auto name = entry.path().filename().string();
    if (name.size() <= 10 || !name.ends_with(".ninjasave")) {
      continue;
    }
    ++found;
    REXLOG_INFO("  save package: {}", entry.path().string());
  }
  if (found == 0) {
    REXLOG_INFO("No .ninjasave packages under {} yet", user_data_root.string());
  } else {
    REXLOG_INFO("Found {} Broken Bond save package(s)", found);
  }
}

}  // namespace

std::optional<rex::PathConfig> BrokenBondApp::OnFinalizePaths(
    const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) {
  (void)resume;
  // SetupEnvironment snapshots game_data_root before LoadConfig, so a toml
  // value is ignored unless we re-read the cvar here.
  auto paths = defaults;
  if (paths.game_data_root.empty()) {
    const auto from_cvar = rex::cvar::GetFlagByName("game_data_root");
    if (!from_cvar.empty()) {
      paths.game_data_root = from_cvar;
    }
  }
  return paths;
}

void BrokenBondApp::OnPostSetup() {
  // GPU plugin cvars exist only after xenos is loaded.
  // Black screens were EDRAM aliasing; render_target_path_d3d12=rov (CLI/toml,
  // kInitOnly) fixed them. Do not force occlusion off: fake sample counts can
  // stall scripted waits (cutscene still ticks, VFX loop, timeline stuck).

  const auto& root = user_data_root();
  REXLOG_INFO("Broken Bond HDD/content root: {}", root.string());
  REXLOG_INFO(
      "ReXGlue exposes a dummy HDD (device id 1). The Xbox storage picker is "
      "auto-accepted; saves are STFS packages under this folder, not next to "
      "the game dump.");
  LogExistingSaves(root);
}
