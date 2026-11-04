#include "host/crash_report.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include <rex/cvar.h>

namespace broken_bond::host {

// Android: the system writes a tombstone and logcat shows the backtrace, so
// there is no report of our own. Plume and the Vulkan loader print their
// errors to stderr, which Android drops; keep it as stderr.txt next to the
// log so a failed swap chain or device says why.
void InstallCrashReport() {
  const std::string log_file = rex::cvar::GetFlagByName("log_file");
  if (log_file.empty()) {
    return;
  }
  const std::filesystem::path path = std::filesystem::path(log_file).parent_path() / "stderr.txt";
  if (std::freopen(path.c_str(), "w", stderr)) {
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::fputs("stderr: ready\n", stderr);
  }
}

}  // namespace broken_bond::host
