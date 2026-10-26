# broken_bond - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(broken_bond LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BROKENBOND_SOURCES
    src/main.cpp
    src/broken_bond_app.cpp
    src/compat/xusbcam.cpp
    src/host/boot_flags.cpp
    src/host/d3d_census.cpp
    src/host/device_repack.cpp
    src/host/guest_present.cpp
    src/host/shadow_draw.cpp
)

if(WIN32)
    add_executable(broken_bond WIN32 ${BROKENBOND_SOURCES})
else()
    add_executable(broken_bond ${BROKENBOND_SOURCES})
endif()

# rexgpu-host (--gpu_plugin=host): the SDK's PM4 command processor with a
# backend that draws nothing, so the game's fences and interrupts keep running
# while the native renderer draws. Built from the SDK source tree only.
set(BROKEN_BOND_GPU_PLUGINS xenos)
if(REXSDK_DIR AND EXISTS "${REXSDK_DIR}/src/graphics/command_processor.cpp")
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
    target_link_libraries(rexgpu-host PRIVATE rexruntime xxHash::xxhash)
    list(APPEND BROKEN_BOND_GPU_PLUGINS host)
endif()

rexglue_setup_target(broken_bond GPU_PLUGINS ${BROKEN_BOND_GPU_PLUGINS})

# An SDK built from source links imgui privately into rexruntime, but the
# rex/ui headers the app includes still name imgui.h.
if(TARGET imgui)
    target_include_directories(broken_bond PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# shadow_draw.cpp inlines xxHash (XXH_INLINE_ALL) and needs only its header.
if(TARGET xxHash::xxhash)
    target_include_directories(broken_bond PRIVATE
        $<TARGET_PROPERTY:xxHash::xxhash,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Reads game/xeshader.sdb. Not part of the game link and writes no cache.
# The installed GPU plugin is D3D12-only, so SPIR-V is compiled from the SDK
# checkout when that tree sits next to this repo. The game plugin is not rebuilt.
add_executable(shader_db tools/shader_db.cpp)

set(BROKEN_BOND_REXGLUE_SOURCE "" CACHE PATH
    "rexglue-sdk checkout used to compile the SPIR-V translator into shader_db")
if(BROKEN_BOND_REXGLUE_SOURCE STREQUAL "")
    get_filename_component(_rexglue_sibling
        "${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-sdk" ABSOLUTE)
    if(EXISTS "${_rexglue_sibling}/src/graphics/pipeline/shader/spirv_translator.cpp")
        set(BROKEN_BOND_REXGLUE_SOURCE "${_rexglue_sibling}")
    endif()
endif()

if(EXISTS "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv_translator.cpp")
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
    set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    # An SDK built from source with Vulkan already adds glslang.
    if(NOT TARGET glslang)
        add_subdirectory("${BROKEN_BOND_REXGLUE_SOURCE}/thirdparty/glslang"
            "${CMAKE_CURRENT_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)
    endif()

    set(BROKEN_BOND_TRANSLATOR_SOURCES
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/format/ucode.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/shader.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/translator.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/translator_disasm.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv_builder.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv_translator.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv_translator_alu.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv_translator_fetch.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv_translator_memexport.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/shader/spirv_translator_rb.cpp")
    target_compile_definitions(shader_db PRIVATE SHADER_DB_TRANSLATE)
    target_include_directories(shader_db BEFORE PRIVATE
        "${BROKEN_BOND_REXGLUE_SOURCE}/include"
        "${BROKEN_BOND_REXGLUE_SOURCE}/thirdparty/vulkan-headers/include"
        "${BROKEN_BOND_REXGLUE_SOURCE}/thirdparty/renderdoc")
    target_link_libraries(shader_db PRIVATE
        rex::runtime
        SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler)
    target_compile_options(shader_db PRIVATE -Wno-unused-variable -Wno-unused-function)
endif()

set(BROKEN_BOND_VULKAN_SDK "" CACHE PATH "Vulkan SDK used to validate shader_db SPIR-V")
if(BROKEN_BOND_VULKAN_SDK STREQUAL "" AND
   EXISTS "H:/Tools/VulkanSDK/Include/spirv-tools/libspirv.h")
    set(BROKEN_BOND_VULKAN_SDK "H:/Tools/VulkanSDK")
endif()
if(EXISTS "${BROKEN_BOND_VULKAN_SDK}/Include/spirv-tools/libspirv.h")
    target_include_directories(shader_db PRIVATE "${BROKEN_BOND_VULKAN_SDK}/Include")
    target_compile_definitions(shader_db PRIVATE
        "SHADER_DB_VULKAN_SDK=\"${BROKEN_BOND_VULKAN_SDK}\"")
endif()

# Host renderer. Linked into the game. The default renderer stays Xenos, and
# present stays on Xenos until renderer=host is drawing frames.
if(EXISTS "${BROKEN_BOND_VULKAN_SDK}/Lib/vulkan-1.lib" AND
   EXISTS "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/conversion.cpp")
    add_library(broken_bond_gfx STATIC
        src/gfx/device.cpp
        src/gfx/device_shadow.cpp
        src/gfx/edram.cpp
        src/gfx/guest_stage.cpp
        src/gfx/host_draw.cpp
        src/gfx/shader_object.cpp
        src/gfx/texture.cpp
        src/gfx/vertex_semantic.cpp
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/conversion.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/info_formats.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/util.cpp")
    if(DEFINED BROKEN_BOND_TRANSLATOR_SOURCES)
        target_sources(broken_bond_gfx PRIVATE ${BROKEN_BOND_TRANSLATOR_SOURCES})
    endif()
    target_include_directories(broken_bond_gfx
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
        PRIVATE
            "${BROKEN_BOND_REXGLUE_SOURCE}/include"
            "${BROKEN_BOND_REXGLUE_SOURCE}/thirdparty/vulkan-headers/include"
            "${BROKEN_BOND_REXGLUE_SOURCE}/thirdparty/renderdoc"
            "${BROKEN_BOND_VULKAN_SDK}/Include")
    target_link_libraries(broken_bond_gfx PUBLIC
        rex::runtime
        SPIRV
        glslang
        MachineIndependent
        GenericCodeGen
        OSDependent
        OGLCompiler
        "${BROKEN_BOND_VULKAN_SDK}/Lib/vulkan-1.lib")
    # rex/hash.h includes xxhash.h; the SDK source tree does not export it.
    if(TARGET xxHash::xxhash)
        target_link_libraries(broken_bond_gfx PUBLIC xxHash::xxhash)
    endif()
    target_compile_options(broken_bond_gfx PRIVATE -Wno-unused-variable -Wno-unused-function)

    add_executable(host_gfx tools/host_gfx.cpp src/host/device_repack.cpp)
    target_link_libraries(host_gfx PRIVATE broken_bond_gfx)
    target_link_libraries(broken_bond PRIVATE broken_bond_gfx)
endif()

if(DEFINED BROKEN_BOND_TRANSLATOR_SOURCES AND NOT TARGET broken_bond_gfx AND TARGET shader_db)
    target_sources(shader_db PRIVATE ${BROKEN_BOND_TRANSLATOR_SOURCES})
endif()

# Native renderer foundations: Plume (MIT) for Vulkan / D3D12 and a shader
# cache XenosRecomp (MIT, patched by patches/xenosrecomp) builds from this
# dump's xeshader.sdb. setup.ps1 clones both at pinned commits. The cache is
# derived from game files, so it lives in the build tree only.
set(BROKEN_BOND_PLUME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume")
set(BROKEN_BOND_XENOSRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/xenosrecomp")
set(BROKEN_BOND_SHADER_DB "${CMAKE_CURRENT_SOURCE_DIR}/game/xeshader.sdb")
if(EXISTS "${BROKEN_BOND_PLUME_DIR}/CMakeLists.txt" AND
   EXISTS "${BROKEN_BOND_XENOSRECOMP_DIR}/CMakeLists.txt" AND
   EXISTS "${BROKEN_BOND_SHADER_DB}" AND TARGET shader_db)
    add_subdirectory("${BROKEN_BOND_PLUME_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    add_subdirectory("${BROKEN_BOND_XENOSRECOMP_DIR}/thirdparty/zstd/build/cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)

    # XenosRecomp is a host tool with its own fmt and xxHash, so it builds in
    # its own tree instead of next to the SDK's copies.
    include(ExternalProject)
    set(BROKEN_BOND_XENOSRECOMP_EXE
        "${CMAKE_CURRENT_BINARY_DIR}/xenosrecomp/XenosRecomp/XenosRecomp${CMAKE_EXECUTABLE_SUFFIX}")
    ExternalProject_Add(xenosrecomp_tool
        SOURCE_DIR "${BROKEN_BOND_XENOSRECOMP_DIR}"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/xenosrecomp"
        CMAKE_ARGS
            -G Ninja
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${BROKEN_BOND_XENOSRECOMP_EXE}"
        EXCLUDE_FROM_ALL ON)

    set(BROKEN_BOND_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(BROKEN_BOND_SHADER_CACHE "${BROKEN_BOND_SHADER_DIR}/shader_cache.cpp")
    add_custom_command(
        OUTPUT "${BROKEN_BOND_SHADER_CACHE}"
        COMMAND "$<TARGET_FILE:shader_db>" "${BROKEN_BOND_SHADER_DB}" extract
            "${BROKEN_BOND_SHADER_DIR}/containers"
        COMMAND "${BROKEN_BOND_XENOSRECOMP_EXE}" "${BROKEN_BOND_SHADER_DIR}/containers"
            "${BROKEN_BOND_SHADER_CACHE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/xenos_shader_common.h"
        DEPENDS shader_db xenosrecomp_tool "${BROKEN_BOND_SHADER_DB}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/xenos_shader_common.h"
        COMMENT "Translating xeshader.sdb with XenosRecomp"
        VERBATIM)

    add_library(broken_bond_shader_cache STATIC
        "${BROKEN_BOND_SHADER_CACHE}"
        "${BROKEN_BOND_XENOSRECOMP_DIR}/thirdparty/smol-v/source/smolv.cpp")
    target_include_directories(broken_bond_shader_cache
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${BROKEN_BOND_XENOSRECOMP_DIR}/thirdparty/smol-v/source"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx")
    target_link_libraries(broken_bond_shader_cache PUBLIC libzstd_static)
    target_include_directories(broken_bond_shader_cache PUBLIC
        "${BROKEN_BOND_XENOSRECOMP_DIR}/thirdparty/zstd/lib")

    add_executable(render_probe EXCLUDE_FROM_ALL tools/render_probe.cpp)
    target_link_libraries(render_probe PRIVATE plume broken_bond_shader_cache)

    # The shadow recorder checks each draw's shaders against the cache keys.
    target_link_libraries(broken_bond PRIVATE broken_bond_shader_cache)
    target_compile_definitions(broken_bond PRIVATE BROKEN_BOND_HAS_SHADER_CACHE)

    # Native renderer (--renderer=native with --gpu_plugin=host).
    target_sources(broken_bond PRIVATE src/render/renderer.cpp)
    target_link_libraries(broken_bond PRIVATE plume)
    target_compile_definitions(broken_bond PRIVATE BROKEN_BOND_HAS_NATIVE_RENDERER)
endif()

if(TARGET broken_bond_gfx AND TARGET shader_db)
    target_link_libraries(shader_db PRIVATE broken_bond_gfx)
    target_compile_definitions(shader_db PRIVATE BROKEN_BOND_HOST_GFX)
elseif(TARGET shader_db)
    target_sources(shader_db PRIVATE
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/conversion.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/info_formats.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/util.cpp")
endif()
