#pragma once
#include <Windows.h>
#include <cstdint>
#include <string>
namespace end_scaling {
struct FsrBenchmarkResults {
    wchar_t gpu_name[128]{};
    double gpu_ms[3]{};
    double cpu_submit_ms[3]{};
    std::uint32_t measured_frames = 0;
    std::uint32_t scene_frames = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t feature_level = 0;
    std::uint32_t logical_processors = 0;
    std::uint64_t total_ram_mb = 0;
    std::uint64_t available_ram_mb = 0;
    std::uint64_t dedicated_vram_mb = 0;
    std::uint64_t shared_vram_mb = 0;
};
HRESULT run_fsr_benchmark(const wchar_t* shader_path,
                          FsrBenchmarkResults* results,
                          std::wstring* error) noexcept;
} // namespace end_scaling
