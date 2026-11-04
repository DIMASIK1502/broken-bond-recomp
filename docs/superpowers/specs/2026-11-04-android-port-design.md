# Android port — design

## Intent

Run the recompiled Naruto: The Broken Bond on Android with our native Vulkan
renderer. First in the Android emulator on the user's RTX 4070 PC, then on
the user's phone (Snapdragon 778G, Adreno 642L, 6 GB RAM).

The Windows build must keep working exactly as now. Android code lives behind
`REX_PLATFORM_ANDROID` / CMake options and in new files.

## Milestones

1. **Emulator, first fight.** An x86_64 APK. On first start the user picks
   the game folder (the one with `default.xex`); the game reads from it in
   place. PC keyboard and gamepad through SDL3. Native renderer. Done when the
   user reaches and plays the first fight in the emulator.
2. **Phone.** An arm64-v8a APK. Touch overlay (stick left, buttons right) that
   hides while a gamepad is connected; Bluetooth and USB gamepads. Memory and
   speed on the 778G. Optional later: Turnip via libadrenotools.

## Non-goals (for now)

- Game files inside the APK.
- Xenos renderer on Android.
- Store release, signing beyond a debug key.
- Touch-layout editor (after the overlay works on the phone).

## Anti-goals

- Anything that changes Windows behaviour without an option.
- Copying code from GPL-3.0 reference repos or the closed Storm Generations
  port; ideas only.

## What exists

- SDK (`thirdparty/rexglue-sdk`, v0.10.0): POSIX layers, `REX_PLATFORM_ANDROID`
  in `platform.h`, Android shared memory (`ASharedMemory_create` in
  `memory_posix.cpp`), `linux-arm64` presets, simde for SIMD (NEON on arm64),
  SDL3 window/input (`window_sdl.cpp`, `windowed_app_main_sdl.cpp`).
- CMake picks `REX_PLATFORM_LINUX` for any `UNIX AND NOT APPLE`, so an NDK
  build lands on the Linux path; X11/Wayland are required there and must be
  skipped for Android.
- Web branch (`vulkan-60gz-web-cursor`): the list of SDK files a non-Windows
  host had to touch (`patches/sdk/web-platform.patch`), a host shell, file
  import, audio and input glue. It is a checklist, not code to reuse as is
  (Emscripten/WebGPU specific).

## Approach

1. **Toolchain.** NDK r30 (`D:\LaHaine\Tools\AndroidTools`), CMake from VS
   Build Tools, a new preset `android-x86_64` (later `android-arm64`),
   API level 30 minimum (all-files access, `ASharedMemory`).
2. **Native library.** Build the game, SDK runtime, rexgpu-host and the
   native renderer as one `libmain.so` loaded by SDL3's `SDLActivity`.
   No DLL plugins on Android: link the renderer statically.
3. **Platform gaps.** Make the SDK configure and compile for Android: skip
   X11/Wayland, surface via `VK_KHR_android_surface` (SDL3 creates it),
   fix what bionic lacks. Each fix is an SDK patch in `patches/sdk/`.
4. **Shaders.** Ship the SPIR-V shader cache built on PC inside the library,
   as on Windows. No runtime shader compilation (the web build's weak spot).
5. **Game folder.** A small Java/Kotlin launcher activity: ask for
   `MANAGE_EXTERNAL_STORAGE`, pick a folder, check `default.xex`, pass the path
   to the native side. Saves and logs go to app storage.
6. **APK.** Gradle with the SDL3 Java sources, built from the command line.
   The user installs it into the emulator with `adb install`.

## Open risks

- Guest memory: a 4 GB+ reservation at fixed addresses inside an Android
  process; works in Xenia's Android code, unproven here.
- Emulator Vulkan (gfxstream to the host 4070) may lack features the
  renderer assumes; the web branch's no-BDA path is the fallback idea.
- Build size and link time of the recompiled code under the NDK.
- Adreno 642L driver quirks (milestone 2).

## Left to the builder

Gradle layout, exact preset names, how the launcher passes the path (intent
extra or a config file), log location.
