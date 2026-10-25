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
    src/host/guest_present.cpp
)

if(WIN32)
    add_executable(broken_bond WIN32 ${BROKENBOND_SOURCES})
else()
    add_executable(broken_bond ${BROKENBOND_SOURCES})
endif()

rexglue_setup_target(broken_bond GPU_PLUGINS xenos)

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
    add_subdirectory("${BROKEN_BOND_REXGLUE_SOURCE}/thirdparty/glslang"
        "${CMAKE_CURRENT_BINARY_DIR}/glslang" EXCLUDE_FROM_ALL)

    target_sources(shader_db PRIVATE
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

# Host renderer. Not linked into broken_bond, so the running game stays on Xenos.
if(EXISTS "${BROKEN_BOND_VULKAN_SDK}/Lib/vulkan-1.lib" AND
   EXISTS "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/conversion.cpp")
    add_library(broken_bond_gfx STATIC
        src/gfx/device.cpp
        src/gfx/guest_stage.cpp
        src/gfx/texture.cpp
        src/gfx/vertex_semantic.cpp
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/conversion.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/info_formats.cpp"
        "${BROKEN_BOND_REXGLUE_SOURCE}/src/graphics/pipeline/texture/util.cpp")
    target_include_directories(broken_bond_gfx
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
        PRIVATE
            "${BROKEN_BOND_REXGLUE_SOURCE}/include"
            "${BROKEN_BOND_VULKAN_SDK}/Include")
    target_link_libraries(broken_bond_gfx PUBLIC
        rex::runtime
        "${BROKEN_BOND_VULKAN_SDK}/Lib/vulkan-1.lib")
    target_compile_options(broken_bond_gfx PRIVATE -Wno-unused-variable -Wno-unused-function)

    add_executable(host_gfx tools/host_gfx.cpp)
    target_link_libraries(host_gfx PRIVATE broken_bond_gfx)
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
