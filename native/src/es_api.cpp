#include <Windows.h>
#include "end_scaling/d3d11_scaler.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cwchar>
#include <iterator>
#include <string>

extern "C" void __cdecl es_install_crash_diagnostics() noexcept;
extern "C" void __cdecl es_crash_set_context(
    std::uint32_t stage, std::int32_t detail) noexcept;

using Microsoft::WRL::ComPtr;

namespace {
void copy_text(wchar_t* destination, int capacity, const std::wstring& value) noexcept {
    if (!destination || capacity <= 0) return;
    wcsncpy_s(destination, static_cast<std::size_t>(capacity), value.c_str(), _TRUNCATE);
}
}

extern "C" {

__declspec(dllexport) std::uint32_t __cdecl es_core_version() noexcept {
    es_install_crash_diagnostics();
    es_crash_set_context(2, S_OK); // native core version entry point
    return 1;
}

__declspec(dllexport) int __cdecl es_probe_d3d11_gpu(
    const wchar_t* shaderPath,
    wchar_t* gpuName,
    int gpuNameCapacity,
    wchar_t* errorText,
    int errorCapacity,
    std::uint32_t* featureLevel) noexcept {

    es_install_crash_diagnostics();
    es_crash_set_context(3, S_OK); // D3D11 adapter/shader probe entered
    if (gpuName && gpuNameCapacity > 0) gpuName[0] = L'\0';
    if (errorText && errorCapacity > 0) errorText[0] = L'\0';
    if (featureLevel) *featureLevel = 0;

    if (!shaderPath || shaderPath[0] == L'\0') {
        copy_text(errorText, errorCapacity, L"FSR shader path is missing.");
        return 0;
    }

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selectedLevel{};
    const HRESULT deviceResult = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 3, D3D11_SDK_VERSION, &device, &selectedLevel, &context);

    if (FAILED(deviceResult) || !device) {
        copy_text(errorText, errorCapacity, L"No compatible hardware Direct3D 11 adapter was found.");
        return 0;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(device.As(&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
        copy_text(gpuName, gpuNameCapacity, adapterDesc.Description);
    }

    end_scaling::D3D11Scaler scaler;
    std::wstring shaderError;
    const HRESULT shaderResult = scaler.initialize(device.Get(), shaderPath, &shaderError);
    if (FAILED(shaderResult)) {
        copy_text(errorText, errorCapacity,
            shaderError.empty() ? L"Direct3D 11 could not compile the scaling shaders." : shaderError);
        return 0;
    }

    if (featureLevel) *featureLevel = static_cast<std::uint32_t>(selectedLevel);
    return 1;
}

} // extern "C"
