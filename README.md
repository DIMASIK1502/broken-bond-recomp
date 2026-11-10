# Naruto: The Broken Bond — Xbox 360 to PC, Android and the browser (ReXGlue)

A static recompilation port of **Naruto: The Broken Bond** (Xbox 360), built
with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue converts the Xbox 360 PowerPC XEX into portable C++ that runs natively:
no emulation of the CPU, no JIT at runtime. On top of that this repository has
its own **native Vulkan renderer**, which draws the game from its own Direct3D
calls instead of emulating the Xenos GPU. The same renderer runs on Windows,
on Android phones and, through WebGPU, in a browser.

This is an unofficial fan project. You must own the original game and supply
your own extracted Xbox 360 files. Game assets are **not** included.

## Status

| Platform | Renderer | State |
|----------|----------|-------|
| Windows x64 | Xenos (ReXGlue GPU emulation, D3D12 or Vulkan) | Playable: menus, story, fights |
| Windows x64 | Native Vulkan (this repo) | Playable: menus, story, fights, cutscenes |
| Android arm64 (Snapdragon) | Native Vulkan, Turnip driver | Fights run at 40-57 FPS on a Snapdragon 778G; known artifacts below |
| Browser (WebAssembly 64, WebGPU) | Native renderer over WebGPU | Boots, menus and story in a local Chromium build; work in progress |

- Keyboard & mouse via ReXGlue `mnk_mode`; gamepads through SDL.
- Saves go to `%USERPROFILE%\Documents\broken_bond\` on Windows and to the
  app's own folder on Android.
- Japanese cutscene voices need the bundled XMA **split-frame hold** SDK patch
  (`rexglue-sdk-v0.10.0.patch`; stock `v0.10.0` hangs at the Hokage coffins).
  The same patch moves a read offset that ends past an XMA buffer on to the
  next one, so a sound cannot loop forever. Rebuild `rexruntime` from the
  patched tree (`-DREXSDK_DIR=thirdparty\rexglue-sdk`).
- Known on Android (Adreno 642L, Turnip): flickering 16×16 colored checkers in
  some menus and, rarely, in fights. Frame dumps show the game's draws are the
  same as in clean frames; the cause is still being looked for.

Iteration notes live in [`docs/PORTING_LOG.md`](docs/PORTING_LOG.md); the
renderer's design and history in [`docs/HOST_RENDERER.md`](docs/HOST_RENDERER.md).

## The native Vulkan renderer

ReXGlue's default GPU path (the Xenos plugin, from Xenia) emulates the Xbox 360
GPU: it reads the command stream the game builds, translates each shader the
first time it is seen and emulates the 10 MB EDRAM render targets. That is
accurate, but heavy, and it needs fragment shader interlock (ROV / FSI) for
correct EDRAM on many GPUs.

The native renderer (`--gpu_plugin=host --renderer=native`,
`src/render/renderer.cpp`) works one level higher. Hooks on the game's own
Direct3D functions (`Draw*`, `Clear`, `Resolve`, `Swap`; mapped in
[`docs/D3D_MAP.md`](docs/D3D_MAP.md)) hand each draw to a renderer built on
[Plume](https://github.com/renderbag/plume) (Vulkan, MIT).

What that gives:

- **No shader translation at runtime.** All 8156 shaders of the game are
  translated ahead of time by [XenosRecomp](https://github.com/hedge-dev/XenosRecomp)
  (with `patches/xenosrecomp/broken-bond.patch`) into SPIR-V, DXIL and WGSL,
  and embedded in the build. A pipeline is only built the first time a state
  combination is drawn.
- **No EDRAM emulation.** Render targets are ordinary Vulkan images, resolves
  are copies, so no ROV / fragment shader interlock is needed. That is what
  lets it run on integrated GPUs and on phones.
- **Less CPU per draw.** Vertex and index data, textures and converted streams
  are cached and only hashed or uploaded again when the game changes them; the
  GPU draws one frame while the next is recorded (two frames in flight).
- **Resolution and quality independent of the guest:** any internal
  resolution (`--native_resolution_scale`), MSAA 1x/2x/4x.
- **Optional 60 FPS** where the game steps at 30: frame interpolation blends
  the in-between frame from the game's own matrices and bones
  (`--native_interpolate`, experimental).
- **One renderer for every platform.** The same code draws on Windows, on
  Android (Turnip or the stock driver, two descriptor sets so Adreno 6xx fits)
  and in the browser (WebGPU through plume-web, a fixed texture heap and WGSL).
- **Debugging built in:** `F9` (or volume-down on Android) writes the next
  frame's draw list and picture to `logs/`; draws can be tinted or skipped by
  shader or vertex count.

The Xenos path stays the default (`--renderer=xenos`) and is untouched; the
native renderer is chosen with flags.

## Running on Windows

Native Vulkan renderer (the build in `out\build\win-amd64-vulkan`, see below):

```powershell
.\out\build\win-amd64-vulkan\broken_bond.exe `
  --game_data_root=game `
  --gpu_plugin=host `
  --renderer=native `
  --mnk_mode
```

Useful options of the native renderer:

| Flag | Default | What it does |
|------|---------|--------------|
| `--native_resolution_scale=2` | `0` (window size) | Internal resolution as a multiple of 1280×720 (0.25-4) |
| `--native_msaa=4` | `4` | Host MSAA samples: 1, 2 or 4 |
| `--native_frames_in_flight=1` | `2` | 1 trades speed for a frame less of latency |
| `--native_interpolate=true` | off | 60 FPS in-between frames where the game runs at 30 (experimental) |
| `--present_interval=1` | `0` | Cap the guest flip interval |
| `--native_dump_frame=N` | `0` | Write frame N's draws to `logs/native-frame-N.txt` (`F9` does the next one) |
| `--log_file=logs\run.log` | | Session log, with 5 s renderer stats |

The phone-oriented options (`--native_record_thread`, `--native_watch_memory`,
`--native_trim_constants`, `--native_flat_maps`, `android_*`) stay off on PC:
they cut the game thread's CPU time on a phone but do not make a desktop GPU
faster, and the first two can add frame-time jitter there.

Xenos GPU emulation instead:

```powershell
.\out\build\win-amd64-relwithdebinfo\broken_bond.exe `
  --game_data_root=game `
  --gpu_plugin=xenos `
  --mnk_mode `
  --render_target_path_d3d12=rov
```

NVIDIA needs `--render_target_path_d3d12=rov` on the Xenos path, or the 3D
world goes black while the UI still draws. With a Vulkan build of Xenos use
`--gpu_backend=vulkan --render_target_path_vulkan=fsi` (`fbo` for GPUs without
interlock). Do not press **F4 "Save to config"** in-game; it can drop such
settings.

## Building

### Prerequisites

- **Windows 10/11 x64**, **Visual Studio 2022** (Windows SDK), **Clang 18+**,
  **CMake 3.25+**, **Ninja**
- **Vulkan SDK** for the native renderer
- Android: NDK 30, platform 36, build-tools 36 and a JDK (see below)
- Browser: [emsdk](https://emscripten.org/) with wasm64 support
- A legally obtained **Naruto: The Broken Bond** Xbox 360 dump

### 1. Set up the SDK and dependencies

```powershell
.\setup.ps1
```

This clones the ReXGlue SDK (pinned to `v0.10.0`) into `thirdparty/rexglue-sdk`
and applies `patches/sdk/` in order: guest present / draw counters and the
boot-movie hook, the XMA fixes, the Android platform layer and the browser
platform layer (each behind its platform's defines). It also clones Plume,
XenosRecomp, libadrenotools (Android) and plume-web (browser). Safe to re-run.

### 2. Provide the game files

Extract the Xbox 360 disc into `game/`, keeping the original layout. The
entrypoint must be `game/default.xex`. `game/` is gitignored; do not commit
ISOs, XEX files or other copyrighted assets.

### 3. Build the SDK CLI (one time)

```powershell
cmake --preset win-amd64-release -DREXSDK_DIR=thirdparty\rexglue-sdk
cmake --build out\build\win-amd64-release --target rexglue
```

Add the built `rexglue.exe` (under `thirdparty\rexglue-sdk\out\win-amd64\Release\`)
to your PATH.

### 4. Windows build with the native renderer

```powershell
cmake --preset win-amd64-relwithdebinfo -B out\build\win-amd64-vulkan `
  -DREXSDK_DIR=thirdparty\rexglue-sdk -DREXGLUE_USE_VULKAN=ON
cmake --build out\build\win-amd64-vulkan --target broken_bond
```

The first build runs `rexglue codegen` (XEX → C++) and translates the shader
database `game/xeshader.sdb` into the embedded shader cache. A plain
`win-amd64-relwithdebinfo` build (D3D12 Xenos) works as before.

### 5. Android (arm64)

The Android build reuses the generated sources and the shader cache of the
Windows build above (codegen, XenosRecomp and DXC are Windows tools), so build
Windows first. Then, with `ANDROID_HOME` pointing at the Android SDK (with the
NDK) and `JAVA_HOME` at a JDK:

```powershell
cmake -G Ninja -B out\build\android-arm64 `
  -DCMAKE_TOOLCHAIN_FILE=$env:ANDROID_HOME\ndk\30.0.16248370\build\cmake\android.toolchain.cmake `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=30 -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DREXSDK_DIR=thirdparty\rexglue-sdk -DREXGLUE_USE_VULKAN=ON
cmake --build out\build\android-arm64 --target main
python tools\android\build_apk.py --build out\build\android-arm64 --abi arm64-v8a
```

The APK (`out\dist\broken-bond-arm64-v8a.apk`, no Gradle involved) opens a
launcher: pick the game folder, the internal resolution, MSAA and, on
Snapdragon, a Turnip driver zip for AdrenoTools (Turnip v26.3.0-R6 from
StevenMXZ/Adreno-Tools-Drivers worked best on a Snapdragon 778G; the stock
Adreno 6xx driver lacks BC textures). The launcher's checkboxes turn on the
phone options listed above. Logs and frame dumps go to
`Android/data/com.lahaine.brokenbond/files/logs/`.

### 6. Browser (local)

```powershell
emsdk_env
cmake --preset web-wasm64-release
cmake --build out\build\web-wasm64-release
python tools\web\serve.py out\build\web-wasm64-release\web
```

Open the printed `127.0.0.1` address in a Chromium-based browser with WebGPU.
The page asks for the game folder once and keeps it in the browser's private
storage (OPFS); nothing is uploaded anywhere. Design:
[`docs/superpowers/specs/2026-10-31-browser-port-design.md`](docs/superpowers/specs/2026-10-31-browser-port-design.md).

## Project layout

```
.
├── CMakeLists.txt                 # Windows build; includes cmake/android or cmake/web
├── CMakePresets.json              # Platform build presets (incl. web-wasm64-release)
├── broken_bond_manifest.toml      # ReXGlue project manifest
├── generated/                     # codegen output (default/ gitignored)
├── src/
│   ├── main.cpp, broken_bond_app.*  # App entry and title hooks
│   ├── host/                      # D3D hooks, guest present, shadow draws
│   ├── render/renderer.cpp        # Native renderer (Plume: Vulkan / WebGPU)
│   ├── gfx/                       # Shader cache contract, texture and vertex formats
│   └── web/                       # Browser host (Emscripten)
├── android/                       # Launcher and game activity (Java), manifest
├── web/                           # Browser page, input, audio, OPFS import
├── cmake/                         # Android and browser build scripts
├── tools/                         # Windows launcher, APK builder, web server
├── patches/
│   ├── apply_sdk_patches.ps1      # git apply against the cloned SDK
│   ├── sdk/                       # SDK patches, applied in order by the script
│   └── xenosrecomp/               # XenosRecomp changes for this game
├── docs/                          # Porting log, D3D map, renderer notes, designs
└── setup.ps1                      # Clone SDK and dependencies, apply patches
```

## Optional Windows launcher

`tools/` contains a small Windows launcher (`launcher_win.cpp`) that writes
`broken_bond.toml` and starts the exe with the required flags for the Xenos
path (`cd tools` then `build_launcher.bat`). Put `Launcher.exe` next to
`broken_bond.exe`, the ReXGlue runtime DLLs and a `game\` folder, then run
`Start.bat` / `Запуск.bat`.

## How to ship a build

This repo is source-only. A playable Windows folder is `broken_bond.exe`, the
ReXGlue runtime and GPU plugin DLLs the build placed next to it
(`rexruntime*.dll`, `rexgpu-*.dll`), optionally the launcher, and the player's
own `game\` folder. **Do not upload** `game\`, ISOs, XEX files or copyrighted
assets, and do not commit `out\`, `logs\` or `generated\default\`.

## Customizing the port

Override hooks in `src/broken_bond_app.h` / `src/broken_bond_app.cpp`
(for example `OnPostSetup`, `OnFinalizePaths`). Those files are user-owned and
kept across `rexglue init` / `rexglue migrate`. Missing guest functions that
trapped at runtime are listed under `[entrypoint.functions]` in
`broken_bond_manifest.toml`.

## AI usage disclosure

This port was built with AI coding assistants ([Cursor](https://cursor.com)
and Claude Code) in the loop, under human direction.

**AI was used for:** title-side C++, the D3D hooks and the native renderer,
the Android and browser ports, runtime diagnosis (missing function-table
entries, black screens, XMA hangs, GPU artifacts), setup scripts and this
README.

**A human owned:** the legally obtained game dump and all in-game testing on
PC, phone and browser; which problems to fix next and whether a change
actually worked; review of what ships in the tree.

The recompiled guest code itself is produced by `rexglue codegen` from your
XEX, not by the assistant.

## License

This repository contains port scaffolding, compatibility code, the native
renderer and configuration. The ReXGlue SDK is licensed under the BSD 3-Clause
License (see `thirdparty/rexglue-sdk/` after setup); Plume, XenosRecomp and
plume-web are MIT, libadrenotools is BSD-2-Clause.

Naruto: The Broken Bond and all game assets are property of their respective
rights holders. Nothing under `game/` is distributed here.
