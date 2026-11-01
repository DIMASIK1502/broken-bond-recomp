# Browser build. The top-level CMakeLists includes this only when
# BROKEN_BOND_WEB=ON under toolchains/web-emscripten.cmake, then returns.
# Native presets never read it.
#
# This target is the page: local OPFS import, the input block, the audio ring,
# and the recompiled guest. The page presents on #canvas through plume-web.

enable_language(C)

set(_root "${CMAKE_CURRENT_SOURCE_DIR}")
set(_web_dir "${CMAKE_BINARY_DIR}/web")

add_compile_options(
    -fno-strict-aliasing
    -ffp-contract=off
    $<$<COMPILE_LANGUAGE:CXX>:-fno-char8_t>
    -Wno-unused-command-line-argument
)

set(PLUME_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${_root}/thirdparty/plume-web" "${CMAKE_BINARY_DIR}/plume-web" EXCLUDE_FROM_ALL)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${_root}/thirdparty/xenosrecomp/thirdparty/zstd/build/cmake"
    "${CMAKE_BINARY_DIR}/zstd" EXCLUDE_FROM_ALL)

add_executable(broken_bond
    src/web/shell_main.cpp
    src/web/web_files.cpp
    src/web/web_input.cpp
    src/web/web_audio.cpp
    src/web/web_picture.cpp
    src/web/web_draw.cpp
    src/web/ui_stub.cpp
    src/render/renderer.cpp
    src/render/interpolation.cpp
    src/host/shadow_draw.cpp
    src/gfx/texture.cpp
    src/compat/xusbcam.cpp
)
target_include_directories(broken_bond PRIVATE
    "${_root}"
    "${_root}/src"
    "${_root}/generated/default"
    "${_root}/thirdparty/rexglue-sdk/include"
    "${_root}/thirdparty/xenosrecomp/thirdparty/zstd/lib")
target_compile_definitions(broken_bond PRIVATE BROKEN_BOND_WEB=1 FMT_HEADER_ONLY)
target_compile_options(broken_bond PRIVATE --use-port=emdawnwebgpu)
target_link_libraries(broken_bond PRIVATE
    broken_bond_web_core
    broken_bond_web_recomp
    broken_bond_web_support
    broken_bond_web_ffmpeg
    plume
    libzstd_static)

set(_page_files
    "${_root}/web/input.js"
    "${_root}/web/import.js"
    "${_root}/web/audio.js"
    "${_root}/web/audio_worklet.js"
    "${_root}/web/shell.html"
)
add_custom_command(TARGET broken_bond POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_web_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_page_files} "${_web_dir}"
    VERBATIM)

target_link_options(broken_bond PRIVATE
    -sPROXY_TO_PTHREAD
    -sPTHREAD_POOL_SIZE=48
    -sINITIAL_MEMORY=3328MB
    -sMAXIMUM_MEMORY=8GB
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8MB
    -sDEFAULT_PTHREAD_STACK_SIZE=2MB
    -sWASMFS
    -sENVIRONMENT=web,worker
    -sEXIT_RUNTIME=1
    -sEXPORTED_RUNTIME_METHODS=callMain,wasmMemory
    -Wno-pthreads-mem-growth
    -Wl,--error-limit=0
    --shell-file "${_root}/web/shell.html"
    --pre-js "${_root}/web/plume_webgpu_device.js"
    --emit-symbol-map
    # wasm-opt -O2 and above overflows its own stack on the recompiled
    # functions. Last -O wins, so this stays after the build type's -O3 and
    # skips that pass. The objects are already LLVM-optimized.
    -O1
    "SHELL:--embed-file ${CMAKE_BINARY_DIR}/shader_cache_wgsl.bin@/shader_cache_wgsl.bin"
)
set_target_properties(broken_bond PROPERTIES
    SUFFIX ".html"
    RUNTIME_OUTPUT_DIRECTORY "${_web_dir}"
)
set_property(TARGET broken_bond APPEND PROPERTY LINK_DEPENDS
    "${_root}/web/shell.html"
    "${_root}/web/plume_webgpu_device.js")

# wasm64 pointers arrive in the Emscripten JS glue as BigInt. The stock glue
# mixes them with numbers and the first present throws. The script edits the
# local emsdk copy and is a no-op once those edits are present. It has to run
# before this link reads library_webgpu.js.
add_custom_command(TARGET broken_bond PRE_LINK
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${_root}/tools/web/patch_emscripten_wasm64.ps1"
    VERBATIM)

# ReXGlue core for the browser. POSIX sources plus src/web/memory_emscripten.cpp.
# The SDK's own CMake is not used: it requires D3D12 or Vulkan. Desktop builds
# keep compiling memory_win.cpp from that CMake.
set(_sdk "${_root}/thirdparty/rexglue-sdk")
if(NOT EXISTS "${_root}/generated/default/sources.cmake")
    message(FATAL_ERROR
        "generated/default/sources.cmake is missing. Build the native game once "
        "so rexglue codegen runs, then configure the browser preset again.")
endif()
if(EXISTS "${_sdk}/src/core/memory.cpp")
    set(_core_posix
        atomic_posix.cpp clock_posix.cpp dbg_posix.cpp dynlib_posix.cpp
        exception_handler_posix.cpp filesystem_posix.cpp fiber_posix.cpp
        mapped_memory_posix.cpp math_gcc.cpp platform/console_posix.cpp
        platform/env_posix.cpp seh_posix.cpp socket_posix.cpp system_posix.cpp
        threading_posix.cpp)
    set(_core_sources
        arena.cpp bit_map.cpp bit_stream.cpp byte_stream.cpp clock.cpp cvar.cpp
        exception_handler.cpp filesystem.cpp filesystem_wildcard.cpp hash.cpp
        logging.cpp memory.cpp mutex.cpp ring_buffer.cpp string_buffer.cpp
        threading.cpp timer_queue.cpp utf8.cpp vec128.cpp)
    list(TRANSFORM _core_sources PREPEND "${_sdk}/src/core/")
    list(TRANSFORM _core_posix PREPEND "${_sdk}/src/core/")
    add_library(broken_bond_web_core OBJECT
        ${_core_sources}
        ${_core_posix}
        "${_root}/src/web/memory_emscripten.cpp"
        "${_sdk}/src/core/fiber_emscripten.cpp"
        "${_sdk}/src/ui/windowed_app_context.cpp"
        "${_sdk}/src/ui/window.cpp"
        "${_sdk}/src/ui/keybinds.cpp"
        "${_sdk}/src/core/perf/counter.cpp"
        "${_sdk}/src/graphics/xenos.cpp"
        "${_sdk}/src/graphics/pipeline/texture/info.cpp"
        "${_sdk}/src/graphics/pipeline/texture/info_formats.cpp"
        "${_sdk}/src/graphics/pipeline/texture/extent.cpp"
        "${_sdk}/src/graphics/pipeline/texture/util.cpp"
        "${_sdk}/src/graphics/pipeline/texture/conversion.cpp"
        "${_sdk}/src/system/xmemory.cpp")

    set(_tp "${_sdk}/thirdparty")
    set(_fs
        device.cpp entry.cpp virtual_file_system.cpp
        devices/disc_image_device.cpp devices/disc_image_entry.cpp devices/disc_image_file.cpp
        devices/host_path_device.cpp devices/host_path_entry.cpp devices/host_path_file.cpp
        devices/null_device.cpp devices/null_entry.cpp devices/null_file.cpp
        devices/stfs_container_device.cpp devices/stfs_container_entry.cpp
        devices/stfs_container_file.cpp)
    list(TRANSFORM _fs PREPEND "${_sdk}/src/filesystem/")
    set(_system
        runtime.cpp embedded_metadata.cpp achievement_manager.cpp achievements.cpp
        kernel_state.cpp kernel_module.cpp user_module.cpp xthread.cpp xevent.cpp xfile.cpp
        xmutant.cpp xsemaphore.cpp xtimer.cpp xiocompletion.cpp xnotifylistener.cpp
        xenumerator.cpp xsocket.cpp xsymboliclink.cpp xobject.cpp xmodule.cpp guest_path.cpp
        util/object_table.cpp util/native_list.cpp util/xdbf_utils.cpp util/gameinfo_utils.cpp
        xam/app_manager.cpp xam/content_manager.cpp xam/user_profile.cpp
        ppc_types.cpp elf_module.cpp entry_table.cpp export_resolver.cpp lzx.cpp
        mmio_handler.cpp map_parser.cpp module.cpp function_dispatcher.cpp
        gpu_plugin_loader.cpp thread.cpp thread_state.cpp xex_module.cpp)
    list(TRANSFORM _system PREPEND "${_sdk}/src/system/")
    set(_kernel
        kernel_init.cpp
        xam/xam_module.cpp xam/xam_avatar.cpp xam/xam_content.cpp xam/xam_content_aggregate.cpp
        xam/xam_content_device.cpp xam/xam_debug.cpp xam/xam_enum.cpp xam/xam_info.cpp
        xam/xam_input.cpp xam/xam_locale.cpp xam/xam_msg.cpp xam/xam_net.cpp xam/xam_notify.cpp
        xam/xam_nui.cpp xam/xam_party.cpp xam/xam_task.cpp xam/xam_ui.cpp xam/xam_user.cpp
        xam/xam_video.cpp xam/xam_voice.cpp xam/xam_misc.cpp
        xam/apps/xam_app.cpp xam/apps/xgi_app.cpp xam/apps/xlivebase_app.cpp xam/apps/xmp_app.cpp
        xbdm/xbdm_module.cpp xbdm/xbdm_misc.cpp
        xboxkrnl/xboxkrnl_crypt.cpp xboxkrnl/xboxkrnl_debug.cpp xboxkrnl/xboxkrnl_error.cpp
        xboxkrnl/xboxkrnl_hal.cpp xboxkrnl/xboxkrnl_hid.cpp xboxkrnl/xboxkrnl_io.cpp
        xboxkrnl/xboxkrnl_io_info.cpp xboxkrnl/xboxkrnl_memory.cpp xboxkrnl/xboxkrnl_misc.cpp
        xboxkrnl/xboxkrnl_module.cpp xboxkrnl/xboxkrnl_modules.cpp xboxkrnl/xboxkrnl_ob.cpp
        xboxkrnl/xboxkrnl_rtl.cpp xboxkrnl/xboxkrnl_strings.cpp xboxkrnl/xboxkrnl_threading.cpp
        xboxkrnl/xboxkrnl_video.cpp xboxkrnl/xboxkrnl_xconfig.cpp
        xboxkrnl/xboxkrnl_audio.cpp xboxkrnl/xboxkrnl_audio_xma.cpp
        crt/heap.cpp crt/file.cpp crt/memory.cpp crt/string.cpp crt/threading.cpp)
    list(TRANSFORM _kernel PREPEND "${_sdk}/src/kernel/")
    set(_audio
        audio_driver.cpp audio_system.cpp downmix.cpp xma_context.cpp xma_decoder.cpp
        xma_register_file.cpp nop/nop_audio_system.cpp)
    list(TRANSFORM _audio PREPEND "${_sdk}/src/audio/")
    set(_input
        input_system.cpp device_assignment.cpp state_merge.cpp
        mnk/mnk_input_driver.cpp nop/nop_input_driver.cpp)
    list(TRANSFORM _input PREPEND "${_sdk}/src/input/")
    set(_gfx
        flags.cpp register_file.cpp registers.cpp pipeline/shader/shader.cpp format/ucode.cpp
        sampler_info.cpp graphics_system.cpp command_processor.cpp
        util/draw_extent_estimator.cpp util/draw.cpp packet_disassembler.cpp
        pipeline/shader/interpreter.cpp)
    list(TRANSFORM _gfx PREPEND "${_sdk}/src/graphics/")
    target_sources(broken_bond_web_core PRIVATE
        ${_fs} ${_system} ${_kernel} ${_audio} ${_input} ${_gfx}
        "${_root}/src/render/host_gpu_plugin.cpp")

    target_include_directories(broken_bond_web_core PUBLIC
        "${_sdk}/include"
        "${_sdk}/thirdparty"
        "${_sdk}/thirdparty/fmt/include"
        "${_sdk}/thirdparty/spdlog/include"
        "${_sdk}/thirdparty/tomlplusplus/include"
        "${_sdk}/thirdparty/xxHash"
        "${_sdk}/thirdparty/simde"
        "${_sdk}/thirdparty/utfcpp/source"
        "${_sdk}/thirdparty/disruptorplus/include"
        "${_sdk}/thirdparty/cli11/include"
        "${_sdk}/thirdparty/sdl3/include"
        "${_sdk}/thirdparty/imgui"
        "${_sdk}/thirdparty/libmspack/cabextract/mspack"
        "${_sdk}/thirdparty/tiny-aes-c"
        "${_sdk}/thirdparty/o1heap/o1heap"
        "${_sdk}/thirdparty/renderdoc"
        "${_sdk}/thirdparty/stb"
        "${_sdk}/thirdparty/FFmpeg"
        "${_sdk}/src/filesystem/devices"
        "${_root}/src")
    target_compile_definitions(broken_bond_web_core PUBLIC FMT_HEADER_ONLY
        REXGLUE_BUILD_CONFIG="$<CONFIG>")
    target_compile_options(broken_bond_web_core PRIVATE
        -Wno-unused-parameter
        -Wno-deprecated-pragma
        -Wno-unused-variable
        "SHELL:-include ${_root}/src/web/emscripten_preinclude.h")

    add_library(broken_bond_web_support STATIC
        "${_tp}/tiny-aes-c/aes.c"
        "${_tp}/aes_128/aes.c"
        "${_tp}/libmspack/cabextract/mspack/lzxd.c"
        "${_tp}/o1heap/o1heap/o1heap.c")
    target_include_directories(broken_bond_web_support PUBLIC
        "${_tp}/tiny-aes-c" "${_tp}/aes_128" "${_tp}/libmspack/cabextract/mspack"
        "${_tp}/o1heap/o1heap")
    target_compile_definitions(broken_bond_web_support PUBLIC AES128=1 CBC=1)
    target_compile_definitions(broken_bond_web_support PRIVATE
        "O1HEAP_CONFIG_HEADER=\"${_sdk}/include/rex/o1heap_config.h\"")
    target_compile_options(broken_bond_web_support PRIVATE -w)
    include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg.cmake")

    add_library(broken_bond_web_imgui STATIC
        "${_tp}/imgui/imgui.cpp"
        "${_tp}/imgui/imgui_draw.cpp"
        "${_tp}/imgui/imgui_tables.cpp"
        "${_tp}/imgui/imgui_widgets.cpp")
    target_include_directories(broken_bond_web_imgui PUBLIC "${_tp}/imgui")
    target_compile_options(broken_bond_web_imgui PRIVATE -w)
    target_link_libraries(broken_bond PRIVATE broken_bond_web_imgui)

    # Guest code stays off the page until it links. -O1 is enough to surface
    # missing headers; the playable link can raise this later.
    include("${_root}/generated/default/sources.cmake")
    add_library(broken_bond_web_recomp OBJECT ${GENERATED_SOURCES})
    target_include_directories(broken_bond_web_recomp PRIVATE
        "${_sdk}/include"
        "${_sdk}/thirdparty"
        "${_sdk}/thirdparty/fmt/include"
        "${_sdk}/thirdparty/spdlog/include"
        "${_sdk}/thirdparty/tomlplusplus/include"
        "${_sdk}/thirdparty/xxHash"
        "${_sdk}/thirdparty/simde"
        "${_sdk}/thirdparty/utfcpp/source"
        "${_sdk}/thirdparty/disruptorplus/include"
        "${_root}/src"
        "${_root}/generated/default")
    target_compile_definitions(broken_bond_web_recomp PRIVATE FMT_HEADER_ONLY)
    target_compile_options(broken_bond_web_recomp PRIVATE
        -O1
        -Wno-unused-parameter
        -Wno-deprecated-pragma
        -Wno-pass-failed
        "SHELL:-include ${_root}/src/web/emscripten_preinclude.h")
endif()

if(EXISTS "${_root}/thirdparty/plume-web/plume_webgpu.cpp")
    message(STATUS "broken_bond web: plume-web is available for the game link")
else()
    message(STATUS "broken_bond web: thirdparty/plume-web is not cloned yet (setup.ps1). The page shell does not link it.")
endif()
