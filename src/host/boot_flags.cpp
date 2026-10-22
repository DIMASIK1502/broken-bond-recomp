#include <string_view>

#include <rex/cvar.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include "generated/default/broken_bond_init.h"
#include "broken_bond_app.h"

REXCVAR_DEFINE_BOOL(skip_boot_logos, false, "Game",
                    "Refuse Ubisoft.bik and AttractMode*.bik so boot logos do not play");

namespace {

std::string_view BaseName(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) return path;
  return path.substr(slash + 1);
}

bool EqualsIgnoreCase(std::string_view text, std::string_view lower_ascii) {
  if (text.size() != lower_ascii.size()) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != lower_ascii[i]) return false;
  }
  return true;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view lower_ascii) {
  if (text.size() < lower_ascii.size()) return false;
  return EqualsIgnoreCase(text.substr(0, lower_ascii.size()), lower_ascii);
}

bool IsBootMovie(std::string_view path) {
  const std::string_view name = BaseName(path);
  const bool bik = name.size() >= 4 &&
                   EqualsIgnoreCase(name.substr(name.size() - 4), ".bik");
  if (!bik) return false;
  return EqualsIgnoreCase(name, "ubisoft.bik") || StartsWithIgnoreCase(name, "attractmode");
}

bool RefuseBootMovie(std::string_view path) {
  if (!IsBootMovie(path)) return false;
  static bool logged_open = false;
  if (!REXCVAR_GET(skip_boot_logos)) {
    if (!logged_open) {
      logged_open = true;
      REXLOG_INFO("boot movie opened (skip off): {}", BaseName(path));
    }
    return false;
  }
  REXLOG_INFO("boot movie refused: {}", BaseName(path));
  return true;
}

}  // namespace

void BrokenBondApp::LogBootPolicy() {
  rex::filesystem::SetFileOpenHook(&RefuseBootMovie);
  if (REXCVAR_GET(skip_boot_logos)) {
    REXLOG_INFO("skip_boot_logos: Ubisoft.bik and AttractMode*.bik will fail to open");
  }
}
