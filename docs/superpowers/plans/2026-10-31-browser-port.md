# Browser Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run Broken Bond locally in a browser (graphics, audio, keyboard and gamepad) without changing the Windows Plume/Vulkan renderer.

**Architecture:** `BROKEN_BOND_WEB=ON` under the Emscripten toolchain includes `cmake/web/broken_bond_web.cmake` and returns before `generated/rexglue.cmake`, so desktop presets never see it. The page imports the player's game folder into OPFS once. The existing `Renderer` talks to plume-web instead of Vulkan, and a second shader cache speaks WGSL with byte offsets into the upload ring. Guest memory uses the ReXGlue 2.75 GiB window and the same `0xE0000000 + 0x1000` physical fold the native renderer already assumes.

**Tech Stack:** Emscripten (Memory64, pthreads, WasmFS, JSPI on the game link), plume-web WebGPU, the current ReXGlue `v0.10.0` tree plus a narrow web platform patch, the existing native renderer.

**Spec:** `docs/superpowers/specs/2026-10-31-browser-port-design.md`

## Global Constraints

- `BROKEN_BOND_WEB` defaults OFF. Native presets do not set it and must not include `cmake/web/broken_bond_web.cmake`.
- `thirdparty/plume` stays at `0ce184b`. WebGPU is only `thirdparty/plume-web` at `73a662579698c8eb954e0697220a8817cdc64472`.
- Do not apply `trials-hd-recompiled/patches/rexglue-sdk/trials-hd.patch`. It rewrites that project's Vulkan backend.
- The `__spirv__` branch of `src/gfx/xenos_shader_common.h` stays the PC contract (`vk::RawBufferLoad` of buffer device addresses).
- Game files are chosen locally and stored in OPFS. The page server does not expose a game-file URL.
- Serve only on `127.0.0.1` with COOP/COEP. Do not publish the `.wasm`.
- Game-module link flags, once the guest is linked: `-sMEMORY64=1 -pthread -fwasm-exceptions -msimd128 -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=48 -sINITIAL_MEMORY=3328MB -sMAXIMUM_MEMORY=8GB -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB -sMALLOC=mimalloc -sJSPI`.
- Guest fold: `src/web/guest_fold.h`. `HostOffset(0xE0000000) == 0x90001000`.

## Review Focus

- A Windows configure of `win-amd64-relwithdebinfo` must not compile `src/web/*.cpp` or plume-web. The fold test is the only new native target, and it links nothing from the game.
- Import paths reject empty parts, `.`, `..`, backslashes and drive letters before any OPFS write. A hostile folder must not escape `broken_bond/game`.
- `Renderer::Open` on Windows still calls `CreateVulkanInterface` and still rejects a device without descriptor indexing and buffer device address. The WebGPU branch is `__EMSCRIPTEN__` only.
- The audio ring header is 32 bytes and the input block is 4256 bytes, matching `web/audio_worklet.js` and `web/input.js`. A layout drift desynchronizes the page and the worker.
- OPFS import copies the folder once. Reloading the page must not download game files and must not delete `home/` saves.

---

### Task 1: Isolate the web configure

**Files:**
- Modify: `CMakeLists.txt` (gate before `include(generated/rexglue.cmake)`)
- Create: `toolchains/web-emscripten.cmake`
- Create: `cmake/web/broken_bond_web.cmake`
- Modify: `CMakePresets.json`
- Modify: `.gitignore` (`thirdparty/plume-web/`)
- Modify: `setup.ps1` (clone plume-web beside plume, not over it)

**Interfaces:**
- Consumes: Emscripten SDK in `EMSDK`.
- Produces: preset `web-wasm64-release` with `-DBROKEN_BOND_WEB=ON` and toolchain `toolchains/web-emscripten.cmake`. Desktop presets stay unchanged.

- [x] **Step 1: Add the gate**

`option(BROKEN_BOND_WEB ... OFF)`. If it is ON and `EMSCRIPTEN` is not set, `FATAL_ERROR`. If it is ON, `include(cmake/web/broken_bond_web.cmake)` and `return()` before `include(generated/rexglue.cmake)`.

- [x] **Step 2: Toolchain sets the ABI flags first**

`toolchains/web-emscripten.cmake` sets `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` to `-sMEMORY64=1 -pthread -fwasm-exceptions -msimd128`, then includes `${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`. Missing `EMSDK` is a `FATAL_ERROR`.

- [ ] **Step 3: Confirm a desktop preset does not define the web target**

With the SDK present: `cmake --preset win-amd64-relwithdebinfo -DREXSDK_DIR=thirdparty/rexglue-sdk` and `cmake --build out/build/win-amd64-relwithdebinfo --target help`. Expected: `broken_bond` and `web_guest_fold_test` exist, and the help text has no `plume-web` link line.

### Task 2: Guest address fold

**Files:**
- Create: `src/web/guest_fold.h`
- Create: `tools/web_guest_fold_test.cpp`
- Modify: `CMakeLists.txt` (native test target next to `interpolation_test`)

**Interfaces:**
- Consumes: nothing.
- Produces: `broken_bond::web::HostOffset(uint32_t guest_address) -> int64_t`.

- [x] **Step 1: Write the failing checks in `tools/web_guest_fold_test.cpp`**

Assert `HostOffset(0xE0000000) == 0x90001000`, `HostOffset(0xE0001000) == 0x90002000`, `HostOffset(0x7F000000) == 0x90000000`, `HostOffset(0xA0000000) == 0x90000000`, `HostOffset(0x90001000) == 0x80001000`, `HostOffset(0x80000000) == 0x80000000`. Assert `sizeof(WebInputBlock) == 4256` and `offsetof(WebAudioRing, samples) == 32`.

- [x] **Step 2: Run it**

`clang++ -std=c++23 -I src tools/web_guest_fold_test.cpp -o out/web_guest_fold_test.exe && out/web_guest_fold_test.exe`

Expected: `ok`.

### Task 3: Local page shell

**Files:**
- Create: `web/shell.html`, `web/import.js`, `web/input.js`, `web/audio.js`, `web/audio_worklet.js`
- Create: `src/web/web_files.h`, `src/web/web_files.cpp`, `src/web/web_input.h`, `src/web/web_input.cpp`, `src/web/web_audio.h`, `src/web/web_audio.cpp`, `src/web/shell_main.cpp`
- Create: `tools/web/serve.py`
- Modify: `src/render/renderer.cpp` (`__EMSCRIPTEN__` device factory only)

**Interfaces:**
- Consumes: `HostOffset` is not used by the shell. The shell exports `broken_bond_web_input_block` and `broken_bond_web_audio_ring`.
- Produces: OPFS root `broken_bond/game` mounted at `/opfs/broken_bond/game`. `InitializeStorage()` returns false unless `default.xex` is a non-empty file. `WebInputBlock` layout matches `web/input.js` offsets 0, 4, 8, 136, 144, 4240. `WebAudioRing` sample rate 48000, 2 channels, 8192 frames.

- [x] **Step 1: Page imports a folder and never fetches game bytes**

`web/import.js` writes under OPFS `broken_bond/game` via `navigator.storage.getDirectory()`. Required file: `default.xex`. Reject unsafe relative paths. No `fetch` of game data. `tools/web/serve.py` binds `127.0.0.1`, sends COOP/COEP, and has no `/assets` route.

- [x] **Step 2: Build the shell**

`emsdk_env`, then `cmake --preset web-wasm64-release` and `cmake --build out/build/web-wasm64-release`. Expected: `out/build/web-wasm64-release/web/broken_bond.html`.

- [x] **Step 3: Open it**

`python tools/web/serve.py out/build/web-wasm64-release/web`. The page asks for a folder until `default.xex` is stored, then logs that the recompiled game is not linked yet. A second visit skips the copy.

- [x] **Step 4: Keep the Vulkan Open path**

In `Renderer::Open`, call `plume::CreateWebGPUInterface()` only under `__EMSCRIPTEN__`. Otherwise call `plume::CreateVulkanInterface()` and keep the descriptor-indexing and buffer-device-address check. The Emscripten branch returns before `createShader` of SPIR-V, with the reason that drawing waits on the WGSL cache.

### Task 4: Link the recompiled game

**Files:**
- Create: `patches/sdk/web-platform.patch` (narrow: `REX_PLATFORM_EMSCRIPTEN`, `memory_emscripten.cpp`, `rex/memory/web_guest.h` using `HostOffset`, fiber/fma/exception stubs, web audio and input drivers)
- Modify: `cmake/web/broken_bond_web.cmake` (SDK object list from Trials' source list, minus `src/gpu_webgpu`; link `generated/default`)

**Interfaces:**
- Consumes: `generated/default/sources.cmake` from a native codegen. `broken_bond::web::InitializeStorage`.
- Produces: the same `broken_bond.html` now contains the guest. `main` is `src/main.cpp`, not `shell_main.cpp`. `--game_data_root=/opfs/broken_bond/game`.

- [x] **Step 1: Fail the link if codegen has not been run**

The web configure `FATAL_ERROR`s when `generated/default/sources.cmake` is missing, and says to build the native target first.

- [x] **Step 2: Guest window comes from `AllocateGuestWindow`**

The SDK's web memory uses `HostOffset` for the `0xE0` view. A native rebuild of `web_guest_fold_test` still prints `ok`.

### Task 5: Draw with the existing renderer

**Files:**
- Modify: `cmake/web/broken_bond_web.cmake` (link `thirdparty/plume-web` and `src/render/renderer.cpp` only on this target)
- Modify: `src/gfx/xenos_shader_common.h` (new branch beside `__spirv__`, PC branch untouched)
- Modify: `src/render/renderer.cpp` (under `__EMSCRIPTEN__`, push byte offsets and `RenderShaderFormat::WGSL`)

**Interfaces:**
- Consumes: `CreateWebGPUInterface()`, canvas selector `#canvas`, plume-web push constants as a storage buffer.
- Produces: `Renderer::Open` completes on the web build. Present samples the guest frontbuffer.

- [ ] **Step 1: Generate a WGSL cache without touching the SPIR-V one**

A host tool writes `shader_cache_wgsl` from the same `xeshader.sdb`. The PC cache file and `CreateVulkanInterface` path stay as they are.

- [ ] **Step 2: Open no longer returns the WGSL placeholder**

Remove the early return added in Task 3. Capability check on the web: shader format WGSL. Do not require `bufferDeviceAddress`.

### Task 6: Sound and controls

**Files:**
- Modify: `src/web/web_audio.cpp` (SDK driver owns the ring; the worklet keeps the same header)
- Modify: `src/web/web_input.cpp` (feed the SDK web input driver from `WebInputBlock`)

**Interfaces:**
- Consumes: `broken_bond_web_audio_ring`, `broken_bond_web_input_block`.
- Produces: XMA/PCM reaches the worklet. Keyboard (`mnk`) and standard gamepad reach player one.

- [ ] **Step 1: A gesture resumes `AudioContext`**

Until then the driver drops frames at the playback rate and does not block the guest.

- [ ] **Step 2: Play**

Menu, a story fight, and a cutscene: picture, sound, and control. `web_guest_fold_test` still prints `ok`.
