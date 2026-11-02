# End-scaling

A lightweight Windows game-upscaling prototype built as a Rust application with a native C++ / Direct3D 11 graphics core.

> **Current state:** the Windows Graphics Capture → Direct3D 11 scaling pipeline builds on Windows and supports both a separate preview and an experimental click-through output overlay aligned to the selected window. The overlay is still a compatibility prototype. It does not change a game's internal render resolution, and no FPS improvement is claimed until the game is configured to render fewer pixels and the full path is measured on real hardware.

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
5. Direct3D 11 scales that GPU texture into the preview or overlay window's swap chain.
6. In overlay mode, a lightweight window-position timer keeps the output aligned with the source window and hides it when the game is not foreground.

The preview remains separate from the source game. Overlay mode places the scaled output over the selected window, but its compatibility and input behavior still require testing with ordinary windowed games.

## Scaling modes

- **Performance:** one bilinear Direct3D 11 pass, with no intermediate texture.
- **Balanced:** AMD FidelityFX Super Resolution 1 EASU + RCAS with reduced sharpening.
- **Quality:** AMD FidelityFX Super Resolution 1 EASU + RCAS with stronger sharpening.

The FSR intermediate is one reusable 32-bit texture. Shader programs are compiled once during initialization. A short-lived shader-resource view is created for the current Windows Graphics Capture frame and released before that frame is returned to the pool.

AMD FSR 1 source is vendored from GPUOpen-Effects/FidelityFX-FSR under its MIT license. Original license notices are kept in native/third_party/amd_fsr1/.

FSR 1 is a spatial upscaler, not frame generation. It can help only when the game itself renders fewer pixels and the app's capture/presentation overhead is lower than the cost saved by that lower render resolution.

## Preview diagnostics

While preview is running, the main window reports:

- **Output FPS:** a rolling rate calculated from successfully presented output frames, not the game's own FPS. Its sampling window resets after the output becomes visible again, so time spent Alt+Tabbed does not dilute the next active measurement.
- **GPU scale:** the latest completed D3D11 timestamp measurement around the scaling passes. A four-slot query ring is polled without flushing or waiting for the GPU; if a query is not ready, it is checked again on a later frame. This excludes desktop composition and game rendering.
- **CPU submit:** CPU wall-clock time spent in the scaler draw call and the non-blocking Present call. It is not GPU execution time.
- **Frames:** total output frames successfully presented in this session.
- **Skipped:** frames rejected by DXGI back-pressure or presentation errors.
- **Hidden:** capture callbacks where the output was hidden/minimized and scaling/presentation were skipped.

The GPU timer uses a small fixed set of timestamp queries and never performs a blocking readback. DXGI back-pressure is counted rather than waited on, and hidden callbacks are tracked separately. All statistics remain diagnostic: output FPS is not the game's FPS.

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
- The selected window can be captured into a separate, resizable preview or experimental overlay output. Scaling profiles can be changed while capture is active; the native renderer reads the selected mode atomically without restarting the capture session.
- The selected profile generates a suggested lower in-game resolution based on the selected window's client size.
- The app reports asynchronously sampled GPU scaling time alongside CPU submission time.
- Profile selection is saved under %APPDATA%\\EndScaling\\settings.ini.

The suggested resolution is informational. The optional window-size controller reduces the selected window's client area, but it does not change a game's internal render-scale setting or inject into games. Some games follow client-area changes and render fewer pixels; others enforce a fixed internal resolution and may ignore or reject the resize.

## Experimental window render-size control

The **Применить низкое разрешение** action reduces the selected ordinary windowed application's client area according to the active profile (Quality 77%, Balanced 67%, Performance 50%). Dimensions are aligned to multiples of eight and never reduced below 320 × 240. The app saves the original outer window rectangle before resizing.

- Use a normal, unmaximized window; minimized and maximized windows are rejected.
- Changing the profile while the reduced size is active recalculates the client dimensions from the saved original size.
- **Вернуть размер**, **Остановить**, closing the output preview, or exiting End-scaling restores the original window rectangle.
- A recovery marker is written under %APPDATA%\\EndScaling\\window_restore.ini before resizing. If End-scaling exits unexpectedly, the next launch attempts recovery only when the same window handle and process ID still match and the window remains smaller at the saved position.
- This changes the physical client size; it is not a game-specific render-scale API. Some games ignore or constrain window resizing.
- The enlarged capture is still shown in a separate preview window. The experimental overlay does not remap mouse coordinates from the enlarged output to the smaller source window, so this is not yet a finished full-screen play-through path.

This is a foundation for reducing source-image render dimensions, not a guarantee of higher game FPS.

## Experimental game overlay

The UI has a separate **Тестовый оверлей** action. It creates a borderless topmost output window aligned to the selected game's window, keeps it hidden while another app is foreground, and uses a non-activating click-through window style so pointer input can reach the game beneath it. The position is checked by a low-frequency UI timer and only updated when the source window actually moves or resizes. When the overlay is hidden after Alt+Tab (or a preview is minimized), captured frames are released without running the scaler or presenting, avoiding unnecessary GPU work while inactive.

- Start with a normal windowed or borderless-window game; switch back to the game after enabling the overlay.
- Press **Ctrl+Alt+F9** at any time to stop capture and remove the overlay.
- This is experimental. Input pass-through and visual alignment still need hands-on testing on Windows 10/11 and on the target GPU.
- Do not use this mode with exclusive-fullscreen or anti-cheat-protected games until compatibility has been verified.

The overlay only replaces the visible presentation with the captured/scaled frame. It does **not** lower the game's render resolution automatically, and its output FPS is not the game's FPS. No performance gain is claimed yet.

## Remaining work before claiming an FPS gain

- The optional window-size controller changes the client area of ordinary windowed apps, but some games may keep rendering at a fixed internal resolution. Game-specific render-scale control is not implemented.
- Full-size presentation with reliable mouse-coordinate remapping, HDR, exclusive-fullscreen, protected-content, and device-loss handling still need implementation and real Windows testing.
- End-to-end game frame-time, latency, and GPU-load measurements are still needed on Intel Iris Xe.

Until the game is demonstrably rendering fewer pixels and these measurements show a net benefit, **no FPS gain is claimed**. Do not test with anti-cheat-protected games yet.
