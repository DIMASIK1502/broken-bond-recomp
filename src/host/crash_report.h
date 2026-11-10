#pragma once

namespace broken_bond::host {

// Writes logs/crash-<time>.txt (exception code, address, module and offset)
// and logs/crash-<time>.dmp (a minidump for the matching PDB) when the
// process dies on an unhandled exception, abort or std::terminate.
void InstallCrashReport();

}  // namespace broken_bond::host
