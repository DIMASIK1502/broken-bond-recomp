# Emscripten toolchain for the browser build.
#
# The memory and thread flags have to be visible before Emscripten.cmake runs:
# it reads CMAKE_C_FLAGS to choose wasm32 or wasm64, and every object must
# agree or wasm-ld refuses to link.
#   -sMEMORY64        64-bit linear memory for the 2.75 GiB guest window
#   -pthread          guest threads are workers on one SharedArrayBuffer
#   -fwasm-exceptions C++ exceptions (toml++, the runtime)
#   -msimd128         SIMD128; SIMDe lowers the recompiled VMX code onto it

if(NOT DEFINED EMSDK)
    if(DEFINED ENV{EMSDK})
        set(EMSDK "$ENV{EMSDK}")
    else()
        message(FATAL_ERROR
            "The browser build needs the Emscripten SDK.\n"
            "Install it from https://emscripten.org/docs/getting_started/downloads.html\n"
            "and run emsdk_env.bat (or emsdk_env.sh) so EMSDK is set.")
    endif()
endif()

set(_BROKEN_BOND_WEB_FLAGS "-sMEMORY64=1 -pthread -fwasm-exceptions -msimd128")
set(CMAKE_C_FLAGS_INIT "${_BROKEN_BOND_WEB_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_BROKEN_BOND_WEB_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_BROKEN_BOND_WEB_FLAGS}")
set(CMAKE_C_FLAGS "${_BROKEN_BOND_WEB_FLAGS}" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "${_BROKEN_BOND_WEB_FLAGS}" CACHE STRING "" FORCE)

include("${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
