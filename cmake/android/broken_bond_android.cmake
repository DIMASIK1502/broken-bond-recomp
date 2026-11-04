# Android build. The top-level CMakeLists includes this only under the NDK
# toolchain, then returns. Native Windows presets never read it.
#
# Output is libmain.so, which SDL3's SDLActivity loads and enters through
# SDL_main, plus librexgpu-host.so, which the runtime opens by name from the
# APK's native library folder. Nothing runs on the build machine: the
# recompiled sources (generated/default) and the shader cache come from a
# Windows build of the same tree, because rexglue codegen, shader_db,
# XenosRecomp and DXC are Windows tools.
# docs/superpowers/specs/2026-11-04-android-port-design.md

set(_root "${CMAKE_CURRENT_SOURCE_DIR}")

if(NOT REXSDK_DIR)
    set(REXSDK_DIR "${_root}/thirdparty/rexglue-sdk")
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)

set(BROKEN_BOND_HOST_SHADER_DIR "${_root}/out/build/win-amd64-vulkan/shaders" CACHE PATH
    "shaders/ folder of a Windows build: shader_cache.cpp and the native renderer's pass headers")
foreach(_file shader_cache.cpp present_vs.h present_ps.h depth_resolve_vs.h depth_resolve_ps.h)
    if(NOT EXISTS "${BROKEN_BOND_HOST_SHADER_DIR}/${_file}")
        message(FATAL_ERROR
            "${BROKEN_BOND_HOST_SHADER_DIR}/${_file} is missing. Build broken_bond on Windows "
            "(out/build/win-amd64-vulkan) first, or point BROKEN_BOND_HOST_SHADER_DIR at its shaders/.")
    endif()
endforeach()
if(NOT EXISTS "${_root}/generated/default/sources.cmake")
    message(FATAL_ERROR "generated/default/sources.cmake is missing. Build the Windows game once first.")
endif()

# Recompiled guest code.
include("${_root}/generated/default/sources.cmake")
add_library(broken_bond_recomp OBJECT ${GENERATED_SOURCES})
target_include_directories(broken_bond_recomp PRIVATE
    "${_root}" "${_root}/src" "${_root}/generated/default")
target_link_libraries(broken_bond_recomp PRIVATE rex::runtime)
target_precompile_headers(broken_bond_recomp PRIVATE "${_root}/generated/default/broken_bond_pch.h")
target_compile_options(broken_bond_recomp PRIVATE -gline-tables-only)
rexglue_apply_target_settings(broken_bond_recomp)

# Plume (Vulkan) and the shader cache translated on Windows.
add_subdirectory("${_root}/thirdparty/plume" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${_root}/thirdparty/xenosrecomp/thirdparty/zstd/build/cmake"
    "${CMAKE_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)

add_library(broken_bond_shader_cache STATIC
    "${BROKEN_BOND_HOST_SHADER_DIR}/shader_cache.cpp"
    "${_root}/thirdparty/xenosrecomp/thirdparty/smol-v/source/smolv.cpp")
target_include_directories(broken_bond_shader_cache
    PUBLIC
        "${_root}/src"
        "${_root}/thirdparty/xenosrecomp/thirdparty/smol-v/source"
        "${_root}/thirdparty/xenosrecomp/thirdparty/zstd/lib"
    PRIVATE "${_root}/src/gfx")
target_link_libraries(broken_bond_shader_cache PUBLIC libzstd_static)

# Host renderer helpers (texture loading, the retained Vulkan device that
# guest_present.cpp opens for --renderer=host) and the SDK's SPIR-V
# translator, as broken_bond_gfx on Windows. Vulkan comes from the system
# libvulkan.so instead of the SDK's vulkan-1.lib.
set(_rex_src "${REXSDK_DIR}/src/graphics")
add_library(broken_bond_gfx STATIC
    src/gfx/device.cpp
    src/gfx/device_shadow.cpp
    src/gfx/edram.cpp
    src/gfx/guest_stage.cpp
    src/gfx/host_draw.cpp
    src/gfx/shader_object.cpp
    src/gfx/texture.cpp
    src/gfx/vertex_semantic.cpp
    "${_rex_src}/pipeline/texture/conversion.cpp"
    "${_rex_src}/pipeline/texture/info_formats.cpp"
    "${_rex_src}/pipeline/texture/util.cpp"
    "${_rex_src}/format/ucode.cpp"
    "${_rex_src}/pipeline/shader/shader.cpp"
    "${_rex_src}/pipeline/shader/translator.cpp"
    "${_rex_src}/pipeline/shader/translator_disasm.cpp"
    "${_rex_src}/pipeline/shader/spirv.cpp"
    "${_rex_src}/pipeline/shader/spirv_builder.cpp"
    "${_rex_src}/pipeline/shader/spirv_translator.cpp"
    "${_rex_src}/pipeline/shader/spirv_translator_alu.cpp"
    "${_rex_src}/pipeline/shader/spirv_translator_fetch.cpp"
    "${_rex_src}/pipeline/shader/spirv_translator_memexport.cpp"
    "${_rex_src}/pipeline/shader/spirv_translator_rb.cpp")
target_include_directories(broken_bond_gfx
    PUBLIC "${_root}/src"
    PRIVATE
        "${REXSDK_DIR}/include"
        "${REXSDK_DIR}/thirdparty/vulkan-headers/include"
        "${REXSDK_DIR}/thirdparty/renderdoc")
target_link_libraries(broken_bond_gfx PUBLIC
    rex::runtime xxHash::xxhash
    SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler
    vulkan)
target_compile_options(broken_bond_gfx PRIVATE -Wno-unused-variable -Wno-unused-function)

# The game: SDL entry point, app, host hooks and the native renderer.
add_library(main SHARED
    "${REXGLUE_SHARE_DIR}/windowed_app_main_sdl.cpp"
    "${REXGLUE_SHARE_DIR}/rex_app.cpp"
    src/main.cpp
    src/broken_bond_app.cpp
    src/compat/xusbcam.cpp
    src/host/boot_flags.cpp
    src/host/crash_report_none.cpp
    src/host/d3d_census.cpp
    src/host/device_repack.cpp
    src/host/guest_present.cpp
    src/host/shadow_draw.cpp
    src/render/renderer.cpp
    src/render/interpolation.cpp)
target_include_directories(main PRIVATE
    "${_root}" "${_root}/src" "${_root}/generated/default" "${BROKEN_BOND_HOST_SHADER_DIR}"
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:xxHash::xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(main PRIVATE
    REXGLUE_BUILD_CONFIG="$<CONFIG>"
    BROKEN_BOND_HAS_SHADER_CACHE
    BROKEN_BOND_HAS_NATIVE_RENDERER)
target_link_libraries(main PRIVATE
    broken_bond_recomp
    rex::runtime
    plume
    broken_bond_gfx
    broken_bond_shader_cache)
rexglue_apply_target_settings(main)

# rexgpu-host (--gpu_plugin=host): the SDK's PM4 command processor with a
# backend that draws nothing, as on Windows.
set(_rex_graphics "${REXSDK_DIR}/src/graphics")
add_library(rexgpu-host SHARED
    src/render/host_gpu_plugin.cpp
    "${_rex_graphics}/flags.cpp"
    "${_rex_graphics}/register_file.cpp"
    "${_rex_graphics}/registers.cpp"
    "${_rex_graphics}/pipeline/shader/shader.cpp"
    "${_rex_graphics}/format/ucode.cpp"
    "${_rex_graphics}/sampler_info.cpp"
    "${_rex_graphics}/graphics_system.cpp"
    "${_rex_graphics}/command_processor.cpp"
    "${_rex_graphics}/util/draw_extent_estimator.cpp"
    "${_rex_graphics}/util/draw.cpp"
    "${_rex_graphics}/packet_disassembler.cpp"
    "${_rex_graphics}/pipeline/shader/interpreter.cpp")
set_target_properties(rexgpu-host PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    DEBUG_POSTFIX "d"
    RELWITHDEBINFO_POSTFIX "rd")
target_include_directories(rexgpu-host PRIVATE "${REXSDK_DIR}/thirdparty/renderdoc")
target_compile_options(rexgpu-host PRIVATE -Wno-unused-variable)
target_link_libraries(rexgpu-host PRIVATE rex::runtime xxHash::xxhash)
add_dependencies(main rexgpu-host)

# libadrenotools (arm64 only): loads a custom Vulkan driver such as Turnip.
# Its hook libraries are loaded by path from the APK's native library folder,
# so they are built next to libmain.so for build_apk.py to pack.
set(_adrenotools "${_root}/thirdparty/libadrenotools")
if(ANDROID_ABI STREQUAL "arm64-v8a" AND EXISTS "${_adrenotools}/CMakeLists.txt")
    add_subdirectory("${_adrenotools}" "${CMAKE_BINARY_DIR}/adrenotools" EXCLUDE_FROM_ALL)
    set(_hooks main_hook file_redirect_hook gsl_alloc_hook hook_impl)
    set_target_properties(${_hooks} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_link_libraries(main PRIVATE adrenotools)
    target_compile_definitions(main PRIVATE BROKEN_BOND_HAS_ADRENOTOOLS)
    add_dependencies(main ${_hooks})
endif()
