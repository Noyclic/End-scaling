#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>

namespace end_scaling {

void* start_window_capture(HWND sourceWindow,
                           HWND outputWindow,
                           std::uint32_t mode,
                           const wchar_t* shaderPath,
                           std::wstring* error) noexcept;

void stop_window_capture(void* handle) noexcept;

} // namespace end_scaling
