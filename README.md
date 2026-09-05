# Naruto: The Broken Bond — Xbox 360 to PC (ReXGlue)

A static recompilation port of **Naruto: The Broken Bond** (Xbox 360) to native PC,
built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue converts Xbox 360 PowerPC XEX executables into portable C++ that runs
natively on Windows (D3D12) — no emulation, no JIT at runtime.

This is an unofficial fan project. You must own the original game and supply
your own extracted Xbox 360 files. Game assets are **not** included.

## Status

Playable work-in-progress on Windows / D3D12 with the Xenos GPU plugin:

- Title boots into the menu; New Game, story, and combat run
- Keyboard & mouse via ReXGlue `mnk_mode`
- Saves go to `%USERPROFILE%\Documents\broken_bond\`
- NVIDIA needs **D3D12 ROV** (`render_target_path_d3d12=rov`) or the 3D world
  goes black while UI still draws (EDRAM aliasing)
- Japanese cutscene voice hang is an XMA end-of-stream issue in ReXGlue; English
  voices are reliable. A local SDK patch (drained EOS) is required for JP lines
  to finish — it is not upstream in stock `v0.10.0`

Iteration notes live in [`docs/PORTING_LOG.md`](docs/PORTING_LOG.md).

## AI usage disclosure

This port was built in [Cursor](https://cursor.com) with an AI coding assistant
in the loop, under human direction.

**AI was used for:**

- Title-side C++ (Vision Camera stubs, path/save logging, CMake wiring)
- Runtime diagnosis: missing PPC function-table entries, GPU black screens,
  shader hitching, XMA cutscene hangs
- Optional Windows launcher (Qt / native Win32 sources under `tools/`)
- Repository hygiene, setup scripts, and this README

**A human owned:**

- The legally obtained game dump and all in-game testing
- Which problems to fix next and whether a change actually worked
- Review of generated patches and anything that ships in the tree

The recompiled guest code itself is produced by `rexglue codegen` from your XEX,
not by the assistant.

## Project layout

```
.
├── CMakeLists.txt                 # Build config
├── CMakePresets.json              # Platform build presets
├── broken_bond_manifest.toml      # ReXGlue project manifest
├── generated/
│   ├── rexglue.cmake              # SDK boilerplate (auto-generated, do not edit)
│   └── default/                   # codegen output (gitignored, built on demand)
├── src/
│   ├── main.cpp                   # App entry
│   ├── broken_bond_app.h / .cpp   # Title hooks (user-owned)
│   └── compat/xusbcam.cpp         # Xbox Vision Camera stubs (gameplay unused)
├── game/                          # Extracted Xbox 360 files (gitignored)
│   └── default.xex                #   <- entrypoint XEX goes here
├── tools/                         # Optional launcher sources
├── docs/
│   └── PORTING_LOG.md             # Evidence-driven porting notes
├── setup.ps1                      # Clone SDK + init submodules
└── .gitignore
```

## Prerequisites

- **Windows 10/11 x64** (this project targets Windows / D3D12)
- **Clang 18+** (LLVM/Clang on PATH as `clang` / `clang++`)
- **CMake 3.25+**
- **Ninja**
- **Visual Studio 2022** (Windows SDK / D3D12 headers)
- A legally obtained **Naruto: The Broken Bond** Xbox 360 dump

## Getting started

### 1. Set up the SDK

```powershell
.\setup.ps1
```

This clones the ReXGlue SDK (pinned to `v0.10.0`) into `thirdparty/rexglue-sdk`
and initializes its submodules.

### 2. Provide the game files

Extract your ripped Xbox 360 ISO into `game/`. The entrypoint executable must be
at `game/default.xex` (the path set in `broken_bond_manifest.toml`). Keep the
original directory layout for all other assets.

If the dump already lives elsewhere:

```bat
mklink /J game "D:\Xbox360\Naruto Broken Bond"
```

> **Do not commit anything under `game/`** — it contains copyrighted assets used
> locally for recompilation only.

### 3. Build the SDK CLI (one time)

```powershell
cmake --preset win-amd64-release -DREXSDK_DIR=thirdparty\rexglue-sdk
cmake --build out\build\win-amd64-release --target rexglue
```

Add the built `rexglue.exe` to your PATH (it lives under
`thirdparty\rexglue-sdk\out\win-amd64\Release\`).

### 4. Configure and build the port

```powershell
cmake --preset win-amd64-relwithdebinfo -DREXSDK_DIR=thirdparty\rexglue-sdk
cmake --build out\build\win-amd64-relwithdebinfo
```

The build runs `rexglue codegen` (XEX → C++) the first time and whenever inputs
change. Output:

`out\build\win-amd64-relwithdebinfo\broken_bond.exe`

A Release preset also works (`win-amd64-release`); RelWithDebInfo is what this
port has been tested with.

### 5. Run

```powershell
.\out\build\win-amd64-relwithdebinfo\broken_bond.exe `
  --game_data_root=game `
  --gpu_plugin=xenos `
  --mnk_mode `
  --render_target_path_d3d12=rov `
  --log_file=logs\run.log `
  --log_level=info
```

Useful extra flags:

| Flag | Why |
|------|-----|
| `--render_target_path_d3d12=rov` | Required on NVIDIA; without it the world goes black |
| `--mnk_mode` | Keyboard / mouse |
| `--resolution_scale=2` | Internal 2K (2560×1024). First combat compiles extra shaders |
| `--log_file=...` | Write a session log |

Do not press **F4 “Save to config”** in-game — it overwrites comments and can
drop required settings such as ROV.

## Optional launcher

`tools/` contains a small Windows launcher (Win32 `launcher_win.cpp`, or the
PySide6 prototype `launcher.py`) that writes `broken_bond.toml` and starts the
exe with the required flags.

```bat
cd tools
build_launcher.bat
```

Put `Launcher.exe` next to `broken_bond.exe`, the ReXGlue runtime DLLs, and a
`game\` folder, then run `Start.bat` / `Запуск.bat`. Graphics presets:

- **Performance** — native 1280×512, fewer hitches
- **Medium** — native + CAS
- **Quality** — internal 2K (prettier; first fight may hitch while PSOs compile)

## How to ship a build

This repo is source-only. A playable folder is assembled locally:

1. Build `broken_bond.exe` as above.
2. Copy from the CMake output directory:
   - `broken_bond.exe`
   - ReXGlue runtime / GPU plugin DLLs (`rexruntime*.dll`, `rexgpu-xenos*.dll`,
     and anything else the build placed next to the exe)
3. Copy the launcher from `tools/` if you want a GUI start.
4. On the target PC, create `game\` and extract **that user’s** Xbox 360 dump
   into it (`default.xex` plus the original asset layout).
5. First launch will compile D3D12 pipelines; later runs reuse the shader cache
   under the user-data folder.

**Do not upload** `game\`, ISOs, XEX files, or copyrighted assets. **Do not**
commit `out\`, `logs\`, or `generated/default\` — they are local build products.

## Customizing the port

Override hooks in `src/broken_bond_app.h` / `src/broken_bond_app.cpp`
(for example `OnPostSetup`, `OnFinalizePaths`). Those files are user-owned and
kept across `rexglue init` / `rexglue migrate`. Missing guest functions that
trapped at runtime are listed under `[entrypoint.functions]` in
`broken_bond_manifest.toml`.

## License

This repository contains only port scaffolding, compatibility code, and
configuration. The ReXGlue SDK is licensed under the BSD 3-Clause License
(see `thirdparty/rexglue-sdk/` after setup).

Naruto: The Broken Bond and all game assets are property of their respective
rights holders. Nothing under `game/` is distributed here.
