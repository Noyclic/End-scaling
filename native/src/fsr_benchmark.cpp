#include "end_scaling/fsr_benchmark.hpp"
#include "end_scaling/d3d11_scaler.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace end_scaling {
namespace {
constexpr UINT kSourceWidth = 960, kSourceHeight = 540;
constexpr UINT kOutputWidth = 1920, kOutputHeight = 1080;
constexpr UINT kWarmupFrames = 8, kMeasuredFrames = 24;
constexpr DWORD kQueryTimeoutMs = 5000;
std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
    return static_cast<std::uint32_t>(r) |
        (static_cast<std::uint32_t>(g) << 8) |
        (static_cast<std::uint32_t>(b) << 16) | 0xff000000u;
}
std::uint8_t channel(float value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
}
std::vector<std::uint32_t> make_test_scene() {
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kSourceWidth) * kSourceHeight);
    const float horizon = kSourceHeight * 0.54f;
    const float orbX = kSourceWidth * 0.55f, orbY = horizon - 42.0f;
    constexpr float radius = 48.0f;
    for (UINT y = 0; y < kSourceHeight; ++y) {
        for (UINT x = 0; x < kSourceWidth; ++x) {
            const float fy = static_cast<float>(y) / kSourceHeight;
            float r{}, g{}, b{};
            if (static_cast<float>(y) < horizon) {
                r = 19.0f + fy * 22.0f; g = 30.0f + fy * 35.0f; b = 58.0f + fy * 62.0f;
                if ((static_cast<int>(x) + static_cast<int>(y) * 3) % 19 == 0) { r += 14.0f; g += 17.0f; b += 22.0f; }
            } else {
                const float depth = (static_cast<float>(y) - horizon) / (kSourceHeight - horizon);
                r = 20.0f + depth * 26.0f; g = 35.0f + depth * 45.0f; b = 43.0f + depth * 43.0f;
                const float hgrid = depth * depth * 26.0f;
                const float vgrid = (static_cast<float>(x) - kSourceWidth * 0.5f) * (0.012f + depth * 0.16f);
                if (std::fabs(hgrid - std::round(hgrid)) < 0.035f || std::fabs(vgrid - std::round(vgrid)) < 0.035f) {
                    r += 24.0f; g += 47.0f; b += 57.0f;
                }
                if (((x / 8u) + (y / 8u)) % 2u == 0u) { r += 2.0f; g += 3.0f; b += 4.0f; }
            }
            if ((static_cast<int>(x) * 3 + static_cast<int>(y) * 5) % 67 < 2) { r += 36.0f; g += 41.0f; b += 45.0f; }
            const float dx = static_cast<float>(x) - orbX, dy = static_cast<float>(y) - orbY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance < radius) {
                const float light = std::max(0.0f, 1.0f - distance / radius);
                r = 45.0f + light * 130.0f; g = 100.0f + light * 125.0f; b = 170.0f + light * 82.0f;
                if (std::fabs(distance - radius * 0.82f) < 1.3f) { r = 225.0f; g = 242.0f; b = 255.0f; }
            }
            const float tx = std::fabs(static_cast<float>(x) - 0.82f * kSourceWidth);
            const float ty = std::fabs(static_cast<float>(y) - 0.80f * kSourceHeight);
            if ((tx < 34.0f && ty < 2.0f) || (ty < 34.0f && tx < 2.0f)) { r = 245.0f; g = 185.0f; b = 76.0f; }
            pixels[static_cast<std::size_t>(y) * kSourceWidth + x] = rgba(channel(r), channel(g), channel(b));
        }
    }
    return pixels;
}
bool wait_for_query(ID3D11DeviceContext* context, ID3D11Asynchronous* query, void* data, UINT size) noexcept {
    const ULONGLONG deadline = GetTickCount64() + kQueryTimeoutMs;
    for (;;) {
        const HRESULT hr = context->GetData(query, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) return true;
        if (FAILED(hr) || GetTickCount64() >= deadline) return false;
        Sleep(1);
    }
}
HRESULT measure_profile(ID3D11Device* device, ID3D11DeviceContext* context, D3D11Scaler& scaler,
    ID3D11ShaderResourceView* source, ID3D11RenderTargetView* destination,
    ScaleMode mode, float sharpness, double* gpuMs, double* cpuMs) {
    for (UINT i = 0; i < kWarmupFrames; ++i) {
        const HRESULT hr = scaler.draw(source, destination, mode, sharpness);
        if (FAILED(hr)) return hr;
    }
    context->Flush();
    D3D11_QUERY_DESC desc{}; desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    ComPtr<ID3D11Query> disjoint;
    HRESULT hr = device->CreateQuery(&desc, &disjoint);
    if (FAILED(hr)) return hr;
    desc.Query = D3D11_QUERY_TIMESTAMP;
    ComPtr<ID3D11Query> start, end;
    hr = device->CreateQuery(&desc, &start); if (FAILED(hr)) return hr;
    hr = device->CreateQuery(&desc, &end); if (FAILED(hr)) return hr;
    context->Begin(disjoint.Get()); context->End(start.Get());
    const auto cpuStart = std::chrono::steady_clock::now();
    for (UINT i = 0; i < kMeasuredFrames; ++i) {
        hr = scaler.draw(source, destination, mode, sharpness);
        if (FAILED(hr)) { context->End(disjoint.Get()); return hr; }
    }
    const auto cpuEnd = std::chrono::steady_clock::now();
    context->End(end.Get()); context->End(disjoint.Get()); context->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
    UINT64 startTimestamp{}, endTimestamp{};
    if (!wait_for_query(context, disjoint.Get(), &disjointData, sizeof(disjointData)) ||
        !wait_for_query(context, start.Get(), &startTimestamp, sizeof(startTimestamp)) ||
        !wait_for_query(context, end.Get(), &endTimestamp, sizeof(endTimestamp))) return DXGI_ERROR_WAS_STILL_DRAWING;
    if (disjointData.Disjoint || disjointData.Frequency == 0 || endTimestamp <= startTimestamp) return E_FAIL;
    const double gpuTotal = static_cast<double>(endTimestamp - startTimestamp) * 1000.0 / static_cast<double>(disjointData.Frequency);
    const double cpuTotal = std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count();
    *gpuMs = gpuTotal / kMeasuredFrames; *cpuMs = cpuTotal / kMeasuredFrames;
    return S_OK;
}
} // namespace
HRESULT run_fsr_benchmark(const wchar_t* shaderPath, FsrBenchmarkResults* results, std::wstring* error) noexcept {
    if (!results || !shaderPath || shaderPath[0] == L'\\0') return E_INVALIDARG;
    *results = {};
    results->source_width = kSourceWidth; results->source_height = kSourceHeight;
    results->output_width = kOutputWidth; results->output_height = kOutputHeight;
    results->measured_frames = kMeasuredFrames;
    try {
        constexpr std::array<D3D_FEATURE_LEVEL, 3> levels{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; D3D_FEATURE_LEVEL selectedLevel{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION, &device, &selectedLevel, &context);
        if (FAILED(hr) || !device || !context) {
            if (error) *error = L"Не удалось создать аппаратное устройство Direct3D 11. WARP/CPU fallback отключён.";
            return FAILED(hr) ? hr : E_FAIL;
        }
        results->feature_level = static_cast<std::uint32_t>(selectedLevel);
        ComPtr<IDXGIDevice> dxgiDevice; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC adapterDesc{};
        if (SUCCEEDED(device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
            wcsncpy_s(results->gpu_name, std::size(results->gpu_name), adapterDesc.Description, _TRUNCATE);
            results->dedicated_vram_mb = static_cast<std::uint64_t>(adapterDesc.DedicatedVideoMemory) / (1024u * 1024u);
            results->shared_vram_mb = static_cast<std::uint64_t>(adapterDesc.SharedSystemMemory) / (1024u * 1024u);
        }
        MEMORYSTATUSEX memory{}; memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory)) {
            results->total_ram_mb = memory.ullTotalPhys / (1024u * 1024u);
            results->available_ram_mb = memory.ullAvailPhys / (1024u * 1024u);
        }
        SYSTEM_INFO systemInfo{}; GetSystemInfo(&systemInfo);
        results->logical_processors = systemInfo.dwNumberOfProcessors;
        D3D11Scaler scaler; std::wstring shaderError;
        hr = scaler.initialize(device.Get(), shaderPath, &shaderError);
        if (FAILED(hr)) {
            if (error) *error = shaderError.empty() ? L"Не удалось инициализировать FSR 1 для теста." : shaderError;
            return hr;
        }
        const auto scene = make_test_scene();
        D3D11_TEXTURE2D_DESC sourceDesc{};
        sourceDesc.Width = kSourceWidth; sourceDesc.Height = kSourceHeight; sourceDesc.MipLevels = 1; sourceDesc.ArraySize = 1;
        sourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sourceDesc.SampleDesc.Count = 1;
        sourceDesc.Usage = D3D11_USAGE_DEFAULT; sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initialData{}; initialData.pSysMem = scene.data();
        initialData.SysMemPitch = kSourceWidth * sizeof(std::uint32_t);
        ComPtr<ID3D11Texture2D> sourceTexture;
        hr = device->CreateTexture2D(&sourceDesc, &initialData, &sourceTexture);
        if (FAILED(hr)) { if (error) *error = L"Не удалось создать тестовую текстуру."; return hr; }
        ComPtr<ID3D11ShaderResourceView> sourceView;
        hr = device->CreateShaderResourceView(sourceTexture.Get(), nullptr, &sourceView); if (FAILED(hr)) return hr;
        D3D11_TEXTURE2D_DESC outputDesc{};
        outputDesc.Width = kOutputWidth; outputDesc.Height = kOutputHeight; outputDesc.MipLevels = 1; outputDesc.ArraySize = 1;
        outputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; outputDesc.SampleDesc.Count = 1;
        outputDesc.Usage = D3D11_USAGE_DEFAULT; outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> outputTexture;
        hr = device->CreateTexture2D(&outputDesc, nullptr, &outputTexture);
        if (FAILED(hr)) { if (error) *error = L"Не удалось создать выходную текстуру теста."; return hr; }
        ComPtr<ID3D11RenderTargetView> outputView;
        hr = device->CreateRenderTargetView(outputTexture.Get(), nullptr, &outputView); if (FAILED(hr)) return hr;
        constexpr std::array<ScaleMode, 3> modes{ScaleMode::quality, ScaleMode::balanced, ScaleMode::performance};
        constexpr std::array<float, 3> sharpness{0.0f, 1.5f, 1.5f};
        for (std::size_t i = 0; i < modes.size(); ++i) {
            hr = measure_profile(device.Get(), context.Get(), scaler, sourceView.Get(), outputView.Get(),
                modes[i], sharpness[i], &results->gpu_ms[i], &results->cpu_submit_ms[i]);
            if (FAILED(hr)) {
                if (error) *error = L"FSR benchmark did not complete. The GPU may be busy or the driver may have reset.";
                return hr;
            }
        }
        return S_OK;
    } catch (...) {
        if (error) *error = L"Unexpected exception while running the FSR benchmark.";
        return E_FAIL;
    }
}
} // namespace end_scaling
