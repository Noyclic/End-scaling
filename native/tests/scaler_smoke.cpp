#include "end_scaling/d3d11_scaler.hpp"

#include <d3d11.h>
#include <wrl/client.h>
#include <iostream>
#include <string>
#include <vector>

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

    constexpr UINT sourceWidth = 320;
    constexpr UINT sourceHeight = 180;
    constexpr UINT outputWidth = 640;
    constexpr UINT outputHeight = 360;

    std::vector<std::uint32_t> pixels(sourceWidth * sourceHeight, 0xff4080c0u);
    D3D11_TEXTURE2D_DESC sourceDesc{};
    sourceDesc.Width = sourceWidth;
    sourceDesc.Height = sourceHeight;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sourceData{};
    sourceData.pSysMem = pixels.data();
    sourceData.SysMemPitch = sourceWidth * sizeof(std::uint32_t);

    ComPtr<ID3D11Texture2D> sourceTexture;
    HRESULT hr = device->CreateTexture2D(&sourceDesc, &sourceData, &sourceTexture);
    if (FAILED(hr)) {
        std::wcerr << L"Could not create source texture. HRESULT=" << std::hex << hr << L"\n";
        return 5;
    }

    ComPtr<ID3D11ShaderResourceView> sourceView;
    hr = device->CreateShaderResourceView(sourceTexture.Get(), nullptr, &sourceView);
    if (FAILED(hr)) {
        std::wcerr << L"Could not create source SRV. HRESULT=" << std::hex << hr << L"\n";
        return 6;
    }

    D3D11_TEXTURE2D_DESC outputDesc{};
    outputDesc.Width = outputWidth;
    outputDesc.Height = outputHeight;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> outputTexture;
    hr = device->CreateTexture2D(&outputDesc, nullptr, &outputTexture);
    if (FAILED(hr)) {
        std::wcerr << L"Could not create output texture. HRESULT=" << std::hex << hr << L"\n";
        return 7;
    }

    ComPtr<ID3D11RenderTargetView> outputView;
    hr = device->CreateRenderTargetView(outputTexture.Get(), nullptr, &outputView);
    if (FAILED(hr)) {
        std::wcerr << L"Could not create output RTV. HRESULT=" << std::hex << hr << L"\n";
        return 8;
    }

    for (const auto mode : {
        end_scaling::ScaleMode::performance,
        end_scaling::ScaleMode::balanced,
        end_scaling::ScaleMode::quality
    }) {
        hr = scaler.draw(sourceView.Get(), outputView.Get(), mode);
        if (FAILED(hr)) {
            std::wcerr << L"Scaling pass failed. HRESULT=" << std::hex << hr << L"\n";
            return 9;
        }
    }

    D3D11_QUERY_DESC eventDesc{};
    eventDesc.Query = D3D11_QUERY_EVENT;
    ComPtr<ID3D11Query> completion;
    hr = device->CreateQuery(&eventDesc, &completion);
    if (FAILED(hr)) {
        std::wcerr << L"Could not create GPU completion query. HRESULT=" << std::hex << hr << L"\n";
        return 10;
    }

    context->End(completion.Get());
    context->Flush();
    BOOL complete = FALSE;
    HRESULT queryResult = S_FALSE;
    for (int attempt = 0; attempt < 5000 && queryResult == S_FALSE; ++attempt) {
        queryResult = context->GetData(
            completion.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (queryResult == S_FALSE) Sleep(1);
    }
    if (queryResult != S_OK || !complete) {
        std::wcerr << L"GPU did not finish the smoke-test draw commands. HRESULT="
                   << std::hex << queryResult << L"\n";
        return 11;
    }

    const HRESULT deviceStatus = device->GetDeviceRemovedReason();
    if (FAILED(deviceStatus)) {
        std::wcerr << L"D3D11 device was removed during smoke test. HRESULT="
                   << std::hex << deviceStatus << L"\n";
        return 12;
    }

    std::wcout << L"D3D11 scaler rendered Performance, Balanced, and Quality passes on WARP.\n";
    return 0;
}
