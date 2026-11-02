# End-scaling

A lightweight Windows game-upscaling prototype built as a Rust application with a native C++ / Direct3D 11 graphics core.

> **Current state:** the Windows Graphics Capture → Direct3D 11 scaling → preview path is implemented and builds on Windows. It captures a selected application window into a two-buffer GPU frame pool, scales incoming frames, and presents them in a separate preview window. This is an engineering preview, not yet a game-FPS booster: it does not replace the original game window or forward mouse/keyboard input, so the game still renders its own frames at its normal rate.

## Design goals

- Native Win32 UI; no Electron, WebView, browser process, or always-running background service.
- Rust owns settings, window selection, and application orchestration.
- C++ owns capture, Direct3D 11 resources, and the per-frame scaling path.
- Hardware D3D11 only. No silent WARP/CPU fallback in the app.
- Two capture buffers, latest-frame processing, no CPU readback, and no retained references to returned capture frames.
- GPU resources are reused; output and intermediate textures are recreated only when their dimensions change.

## Current pipeline

1. The user selects a visible application window from the list.
2. Windows Graphics Capture targets that HWND via IGraphicsCaptureItemInterop::CreateForWindow.
3. A Direct3D11CaptureFramePool with two buffers is created using CreateFreeThreaded.
4. The callback drains stale queued frames and keeps the newest one to avoid adding a frame queue's worth of latency.
5. Direct3D 11 scales that GPU texture into a separate preview window's swap chain.

The current preview is deliberately separate from the source game. It is useful for validating window capture, aspect/resolution handling, the D3D11 shaders, and frame pacing before adding a full-screen game overlay and safe input routing.

## Scaling modes

- **Performance:** one bilinear Direct3D 11 pass, with no intermediate texture.
- **Balanced:** AMD FidelityFX Super Resolution 1 EASU + RCAS with reduced sharpening.
- **Quality:** AMD FidelityFX Super Resolution 1 EASU + RCAS with stronger sharpening.

The FSR intermediate is one reusable 32-bit texture. Shader programs are compiled once during initialization. A short-lived shader-resource view is created for the current Windows Graphics Capture frame and released before that frame is returned to the pool.

AMD FSR 1 source is vendored from GPUOpen-Effects/FidelityFX-FSR under its MIT license. Original license notices are kept in native/third_party/amd_fsr1/.

FSR 1 is a spatial upscaler, not frame generation. It can help only when the game itself renders fewer pixels and the app's capture/presentation overhead is lower than the cost saved by that lower render resolution.

## Preview diagnostics

While preview is running, the main window reports:

- **Preview FPS:** successfully presented preview frames divided by elapsed capture-session time. This is not the game's own FPS.
- **CPU submit:** CPU wall-clock time spent in the scaler draw call and the non-blocking Present call. This is not GPU execution time and does not include the wait for GPU completion.
- **Frames:** total preview frames successfully presented in this session.

These counters are intentionally lightweight and use relaxed atomics; they do not read back GPU resources or add a sampling thread.

## Build on Windows

Requirements:

- Windows 10 version 1903 (build 18362) or newer; Windows 11 recommended.
- Visual Studio 2022 or newer Build Tools with Desktop development with C++.
- Windows 10/11 SDK with C++/WinRT headers.
- CMake 3.24+.
- Rust stable toolchain.

Run from PowerShell at the repository root:

    .\\scripts\\build.ps1

The packaged application is written to dist/. GitHub Actions builds on pushes to main, compiles the Rust app and C++ core, compiles/tests the D3D11 shader pipeline on WARP, and uploads an End-scaling-Windows artifact.

## Current preview controls

- The app checks for a hardware Direct3D 11 adapter and compiles all shader entry points at startup.
- The window list enumerates visible top-level windows outside End-scaling itself.
- The selected window can be captured into a separate, resizable preview.
- The selected profile generates a suggested lower in-game resolution based on the selected window's client size.
- Profile selection is saved under %APPDATA%\\EndScaling\\settings.ini.

The suggested resolution is informational only; the app does not change game settings or inject into games.

## Experimental game overlay

The UI has a separate **Тестовый оверлей** action. It creates a borderless topmost output window aligned to the selected game's window, keeps it hidden while another app is foreground, and uses a non-activating click-through window style so pointer input can reach the game beneath it. The position is checked by a low-frequency UI timer and only updated when the source window actually moves or resizes. When the overlay is hidden after Alt+Tab (or a preview is minimized), captured frames are released without running the scaler or presenting, avoiding unnecessary GPU work while inactive.

- Start with a normal windowed or borderless-window game; switch back to the game after enabling the overlay.
- Press **Ctrl+Alt+F9** at any time to stop capture and remove the overlay.
- This is experimental. Input pass-through and visual alignment still need hands-on testing on Windows 10/11 and on the target GPU.
- Do not use this mode with exclusive-fullscreen or anti-cheat-protected games until compatibility has been verified.

The overlay only replaces the visible presentation with the captured/scaled frame. It does **not** lower the game's render resolution automatically, and its output FPS is not the game's FPS. No performance gain is claimed yet.

## Not implemented yet

- A full-screen output overlay that replaces the visible game image.
- Mouse-coordinate forwarding / input passthrough to the original game.
- Robust per-game resize, HDR, exclusive-fullscreen, protected-content, and device-loss handling.
- End-to-end frame-time and latency measurements on Intel Iris Xe.

Until overlay and input routing are implemented and measured on real hardware, **no FPS gain is claimed**. The next milestone is a properly synchronized output overlay, tested carefully in ordinary windowed games before attempting any anti-cheat-protected titles.
