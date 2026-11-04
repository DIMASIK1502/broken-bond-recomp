#include "host/crash_report.h"

namespace broken_bond::host {

// Android: the system writes a tombstone and logcat shows the backtrace, so
// there is no report of our own.
void InstallCrashReport() {}

}  // namespace broken_bond::host
