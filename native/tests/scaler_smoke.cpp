#include "end_scaling/d3d11_scaler.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"Usage: end_scaling_smoke <path-to-fsr1.hlsl>\n";
        return 2;
    }

    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selected{};
    const HRESULT createResult = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device, &selected, &context);

    if (FAILED(createResult)) {
        std::wcerr << L"Could not create the WARP D3D11 test device. HRESULT="
                   << std::hex << createResult << L"\n";
        return 3;
    }

    end_scaling::D3D11Scaler scaler;
    std::wstring error;
    const HRESULT initResult = scaler.initialize(device.Get(), argv[1], &error);
    if (FAILED(initResult)) {
        std::wcerr << L"Shader pipeline initialization failed. HRESULT="
                   << std::hex << initResult << L"\n" << error << L"\n";
        return 4;
    }

    std::wcout << L"D3D11 scaler initialized; FSR 1 EASU/RCAS and fast shader compiled.\n";
    return 0;
}
