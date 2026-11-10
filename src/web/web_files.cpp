#include "web/web_files.h"

#if !defined(__EMSCRIPTEN__)
namespace broken_bond::web {
bool InitializeStorage() { return false; }
const std::string& GameRoot() {
  static const std::string empty;
  return empty;
}
}  // namespace broken_bond::web
#else

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <unistd.h>

namespace broken_bond::web {
namespace {

constexpr const char* kStorageRoot = "/opfs/broken_bond";

void TellPage(const char* text, bool error) {
  MAIN_THREAD_EM_ASM(
      {
        if (window.brokenBond && window.brokenBond.filesStatus) {
          window.brokenBond.filesStatus(UTF8ToString($0), !!$1);
        }
      },
      text, int(error));
}

std::string g_game_root;

}  // namespace

const std::string& GameRoot() { return g_game_root; }

bool InitializeStorage() {
  try {
    auto backend = wasmfs_create_opfs_backend();
    if (!backend || wasmfs_create_directory("/opfs", 0777, backend) != 0) {
      TellPage("Cannot mount browser storage.", true);
      return false;
    }
    const std::filesystem::path root(kStorageRoot);
    std::filesystem::create_directories(root / "game");
    std::filesystem::create_directories(root / "home");
    std::filesystem::create_directories(root / "logs");
    const auto xex = root / "game" / "default.xex";
    if (!std::filesystem::is_regular_file(xex) || std::filesystem::file_size(xex) == 0) {
      TellPage("Choose the extracted game folder. It needs default.xex.", true);
      return false;
    }
    if (setenv("HOME", (root / "home").string().c_str(), 1) != 0) {
      TellPage("Cannot set the save directory.", true);
      return false;
    }
    if (chdir(kStorageRoot) != 0) {
      TellPage("Cannot enter browser storage.", true);
      return false;
    }
    g_game_root = (root / "game").string();
    std::cout << "web: game root " << g_game_root << " (" << std::filesystem::file_size(xex)
              << " byte default.xex)\n";
    return true;
  } catch (const std::exception& error) {
    std::string message = std::string("Import/storage failed: ") + error.what();
    TellPage(message.c_str(), true);
    std::cerr << message << "\n";
    return false;
  }
}

}  // namespace broken_bond::web

#endif
