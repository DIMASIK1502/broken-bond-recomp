#include "host/crash_report.h"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>

namespace broken_bond::host {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

volatile LONG g_reported = 0;

// "module+offset" for an address, for llvm-symbolizer with the module's PDB.
void PrintFrame(FILE* f, int index, DWORD64 address) {
  char path[MAX_PATH] = "?";
  HMODULE module = nullptr;
  uintptr_t base = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module)) {
    GetModuleFileNameA(module, path, MAX_PATH);
    base = reinterpret_cast<uintptr_t>(module);
  }
  const char* name = path;
  for (const char* c = path; *c; ++c) {
    if (*c == '\\' || *c == '/') name = c + 1;
  }
  std::fprintf(f, "  #%02d %s+0x%llX\n", index, name, static_cast<unsigned long long>(address - base));
}

// Walks the stack from `context` with the x64 unwind tables.
void PrintStack(FILE* f, CONTEXT context) {
  for (int i = 0; i < 64 && context.Rip; ++i) {
    PrintFrame(f, i, context.Rip);
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (function) {
      void* handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data,
                       &establisher, nullptr);
    } else {
      // Leaf function: the return address is on top of the stack.
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
    }
  }
}

// Everything here runs in a dying process: no allocation-heavy work, no
// logger, only Win32 calls and stack buffers.
void WriteReport(const char* what, DWORD code, void* address, EXCEPTION_POINTERS* pointers) {
  if (InterlockedExchange(&g_reported, 1) != 0) return;
  CreateDirectoryA("logs", nullptr);
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

  char module_path[MAX_PATH] = "?";
  uintptr_t module_base = 0;
  HMODULE module = nullptr;
  if (address && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCSTR>(address), &module)) {
    GetModuleFileNameA(module, module_path, MAX_PATH);
    module_base = reinterpret_cast<uintptr_t>(module);
  }

  char text_path[64];
  std::snprintf(text_path, sizeof(text_path), "logs\\crash-%s.txt", stamp);
  if (FILE* f = std::fopen(text_path, "w")) {
    std::fprintf(f, "%s\ncode %08lX\naddress %p\nmodule %s\noffset %llX\nthread %lu\nstack:\n", what, code,
                 address, module_path,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - module_base),
                 GetCurrentThreadId());
    CONTEXT context{};
    if (pointers) {
      context = *pointers->ContextRecord;
    } else {
      RtlCaptureContext(&context);
    }
    PrintStack(f, context);
    std::fclose(f);
  }
  std::fprintf(stderr, "crash: %s code %08lX at %p (%s)\n", what, code, address, module_path);

  if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll")) {
    auto write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    char dump_path[64];
    std::snprintf(dump_path, sizeof(dump_path), "logs\\crash-%s.dmp", stamp);
    HANDLE file = CreateFileA(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (write_dump && file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), pointers, FALSE};
      write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
                 static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                            MiniDumpWithDataSegs),
                 pointers ? &info : nullptr, nullptr, nullptr);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  }
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
  WriteReport("unhandled exception", pointers->ExceptionRecord->ExceptionCode,
              pointers->ExceptionRecord->ExceptionAddress, pointers);
  return EXCEPTION_CONTINUE_SEARCH;
}

void OnAbort(int) { WriteReport("abort", 0, _ReturnAddress(), nullptr); }

void OnTerminate() {
  WriteReport("std::terminate", 0, _ReturnAddress(), nullptr);
  std::abort();
}

}  // namespace

void InstallCrashReport() {
  SetUnhandledExceptionFilter(OnUnhandledException);
  std::signal(SIGABRT, OnAbort);
  std::set_terminate(OnTerminate);
}

}  // namespace broken_bond::host
