# End-scaling

A lightweight Windows game-upscaling project built as a Rust application with a native C++ / Direct3D 11 graphics core.

**Current state:** the first GPU scaling core and minimal native Windows settings window are in place. The game-capture / presentation path is not connected yet, so this preview does not increase FPS by itself. FPS improvement will only be possible after the app can capture a lower-resolution game frame and present the upscaled result without adding more overhead than the lower render resolution saves.

## Design goals

- Small native Win32 UI; no Electron, WebView, or always-running background service.
- Rust owns application settings and lightweight orchestration.
- C++ owns the performance-critical Direct3D 11 path.
- Hardware D3D11 only for scaling. No silent WARP/CPU fallback.
- Reuse GPU objects and render targets; do not allocate per frame.
- Keep the frame queue and intermediate resources small.

## Scaling core

The C++ core currently includes:

- **Performance:** one bilinear Direct3D 11 pass, with no intermediate texture.
- **Balanced:** AMD FidelityFX Super Resolution 1 EASU + RCAS.
- **Quality:** the same FSR 1 passes with a stronger RCAS setting.

The FSR intermediate is one reusable 32-bit texture, recreated only when output dimensions change. The shader pipeline is compiled once during initialization.

AMD FSR 1 is vendored from GPUOpen-Effects/FidelityFX-FSR under its MIT license. The original notices are kept in native/third_party/amd_fsr1/.

FSR 1 works best with an anti-aliased, display-referred source image. Image quality and performance still need testing on the target GPU; the Performance profile is intentionally provided as a cheaper fallback.

## Build on Windows

Requirements:

- Windows 10 version 1809 or newer (Windows 11 recommended).
- Visual Studio 2022 Build Tools with Desktop development with C++.
- Windows 10/11 SDK.
- CMake 3.24+.
- Rust stable toolchain.

Run from PowerShell at the repository root:

    .\scripts\build.ps1

The packaged application is written to dist/. GitHub Actions also builds on pushes to main and uploads an End-scaling-Windows artifact.

## Current preview

The settings window:

- checks for a hardware Direct3D 11 adapter;
- compiles the fast and FSR shader entry points;
- lets you choose and save a scaling profile;
- estimates a lower in-game render resolution from the current display size.

The selected lower resolution is a recommendation only. The user must set it in the game. Window capture, fullscreen presentation, input passthrough, frame pacing, and per-game automation are still future work.

## Planned next engineering steps

1. Add a low-buffer-count Windows Graphics Capture source for a selected window.
2. Present the captured frame on a borderless output surface and handle game input safely.
3. Add frame pacing, resize/device-loss recovery, and per-frame timing.
4. Benchmark end-to-end FPS and latency on Intel Iris Xe before enabling FSR by default.

No FPS or latency claims will be made until the complete capture-to-display path is measured on real hardware.
