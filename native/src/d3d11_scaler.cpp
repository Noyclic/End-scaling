#include <Windows.h>
#include "end_scaling/d3d11_scaler.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace end_scaling {
namespace {

struct alignas(16) EasuConstants { std::uint32_t con[4][4]{}; };
struct alignas(16) RcasConstants { std::uint32_t con[4]{}; };

std::uint32_t as_uint(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

HRESULT compile_shader(const wchar_t* path, const char* entry, const char* target,
                       ComPtr<ID3DBlob>& bytecode, std::wstring* error) {
    ComPtr<ID3DBlob> diagnostics;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, target, flags, 0, &bytecode, &diagnostics);

    if (FAILED(hr) && error != nullptr) {
        if (diagnostics && diagnostics->GetBufferPointer() != nullptr) {
            const auto* message = static_cast<const char*>(diagnostics->GetBufferPointer());
            const int length = static_cast<int>(diagnostics->GetBufferSize());
            const int required = MultiByteToWideChar(CP_UTF8, 0, message, length, nullptr, 0);
            if (required > 0) {
                error->resize(static_cast<std::size_t>(required));
                MultiByteToWideChar(CP_UTF8, 0, message, length, error->data(), required);
            } else {
                *error = L"Direct3D shader compilation failed.";
            }
        } else {
            *error = L"Could not open the FSR shader file or one of its AMD include files.";
        }
    }
    return hr;
}

HRESULT create_constant_buffer(ID3D11Device* device, UINT byteWidth,
                               ID3D11Buffer** buffer) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = byteWidth;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return device->CreateBuffer(&desc, nullptr, buffer);
}

} // namespace

HRESULT D3D11Scaler::initialize(ID3D11Device* device, const wchar_t* shaderPath,
                                std::wstring* error) {
    reset();
    if (!device || !shaderPath || shaderPath[0] == L'\0') return E_INVALIDARG;

    device_ = device;
    device_->GetImmediateContext(&context_);
    if (!context_) return E_FAIL;

    ComPtr<ID3DBlob> vs, linearPs, easuPs, rcasPs;
    HRESULT hr = compile_shader(shaderPath, "FullscreenVS", "vs_5_0", vs, error);
    if (FAILED(hr)) return hr;
    hr = compile_shader(shaderPath, "LinearPS", "ps_5_0", linearPs, error);
    if (FAILED(hr)) return hr;
    hr = compile_shader(shaderPath, "FsrEasuPS", "ps_5_0", easuPs, error);
    if (FAILED(hr)) return hr;
    hr = compile_shader(shaderPath, "FsrRcasPS", "ps_5_0", rcasPs, error);
    if (FAILED(hr)) return hr;

    hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(linearPs->GetBufferPointer(), linearPs->GetBufferSize(), nullptr, &linearPixelShader_);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(easuPs->GetBufferPointer(), easuPs->GetBufferSize(), nullptr, &easuPixelShader_);
    if (FAILED(hr)) return hr;
    hr = device_->CreatePixelShader(rcasPs->GetBufferPointer(), rcasPs->GetBufferSize(), nullptr, &rcasPixelShader_);
    if (FAILED(hr)) return hr;

    hr = create_states();
    if (FAILED(hr)) return hr;
    hr = create_constant_buffers();
    if (FAILED(hr)) return hr;

    initialized_ = true;
    return S_OK;
}

HRESULT D3D11Scaler::create_states() {
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = device_->CreateSamplerState(&samplerDesc, &sampler_);
    if (FAILED(hr)) return hr;

    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rasterizerDesc, &rasterizer_);
    if (FAILED(hr)) return hr;

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&blendDesc, &blend_);
    if (FAILED(hr)) return hr;

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    return device_->CreateDepthStencilState(&depthDesc, &depthDisabled_);
}

HRESULT D3D11Scaler::create_constant_buffers() {
    HRESULT hr = create_constant_buffer(device_.Get(), sizeof(EasuConstants), &easuConstants_);
    if (FAILED(hr)) return hr;
    return create_constant_buffer(device_.Get(), sizeof(RcasConstants), &rcasConstants_);
}

HRESULT D3D11Scaler::ensure_intermediate(UINT width, UINT height) {
    if (intermediateTexture_) {
        D3D11_TEXTURE2D_DESC current{};
        intermediateTexture_->GetDesc(&current);
        if (current.Width == width && current.Height == height) return S_OK;
    }

    intermediateTexture_.Reset();
    intermediateRtv_.Reset();
    intermediateSrv_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &intermediateTexture_);
    if (FAILED(hr)) return hr;
    hr = device_->CreateRenderTargetView(intermediateTexture_.Get(), nullptr, &intermediateRtv_);
    if (FAILED(hr)) return hr;
    return device_->CreateShaderResourceView(intermediateTexture_.Get(), nullptr, &intermediateSrv_);
}

HRESULT D3D11Scaler::draw_pass(ID3D11ShaderResourceView* source,
                               ID3D11RenderTargetView* destination,
                               ID3D11PixelShader* pixelShader,
                               UINT width, UINT height, ID3D11Buffer* constants) {
    if (!source || !destination || !pixelShader || width == 0 || height == 0) return E_INVALIDARG;

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    context_->OMSetRenderTargets(1, &destination, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    context_->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
    context_->OMSetDepthStencilState(depthDisabled_.Get(), 0);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader, nullptr, 0);
    context_->PSSetShaderResources(0, 1, &source);
    ID3D11SamplerState* sampler = sampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    if (constants) {
        context_->PSSetConstantBuffers(0, 1, &constants);
    } else {
        ID3D11Buffer* nullBuffer = nullptr;
        context_->PSSetConstantBuffers(0, 1, &nullBuffer);
    }
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullView = nullptr;
    context_->PSSetShaderResources(0, 1, &nullView);
    return S_OK;
}

HRESULT D3D11Scaler::draw(ID3D11ShaderResourceView* source,
                          ID3D11RenderTargetView* destination,
                          ScaleMode mode, float rcasSharpnessStops) {
    if (!initialized_ || !source || !destination) return E_INVALIDARG;

    ComPtr<ID3D11Resource> sourceResource;
    source->GetResource(&sourceResource);
    ComPtr<ID3D11Texture2D> sourceTexture;
    HRESULT hr = sourceResource.As(&sourceTexture);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11Resource> destinationResource;
    destination->GetResource(&destinationResource);
    ComPtr<ID3D11Texture2D> destinationTexture;
    hr = destinationResource.As(&destinationTexture);
    if (FAILED(hr)) return hr;
    if (sourceResource.Get() == destinationResource.Get()) return E_INVALIDARG;

    D3D11_TEXTURE2D_DESC sourceDesc{}, destinationDesc{};
    sourceTexture->GetDesc(&sourceDesc);
    destinationTexture->GetDesc(&destinationDesc);
    if (sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
        destinationDesc.Width == 0 || destinationDesc.Height == 0 ||
        sourceDesc.SampleDesc.Count != 1 || destinationDesc.SampleDesc.Count != 1) {
        return E_INVALIDARG;
    }

    const bool upscale = destinationDesc.Width > sourceDesc.Width &&
                         destinationDesc.Height > sourceDesc.Height;
    if (mode == ScaleMode::performance || !upscale) {
        return draw_pass(source, destination, linearPixelShader_.Get(),
                         destinationDesc.Width, destinationDesc.Height, nullptr);
    }

    hr = ensure_intermediate(destinationDesc.Width, destinationDesc.Height);
    if (FAILED(hr)) return hr;

    const float sourceWidth = static_cast<float>(sourceDesc.Width);
    const float sourceHeight = static_cast<float>(sourceDesc.Height);
    const float outputWidth = static_cast<float>(destinationDesc.Width);
    const float outputHeight = static_cast<float>(destinationDesc.Height);

    EasuConstants easu{};
    easu.con[0][0] = as_uint(sourceWidth / outputWidth);
    easu.con[0][1] = as_uint(sourceHeight / outputHeight);
    easu.con[0][2] = as_uint(0.5f * sourceWidth / outputWidth - 0.5f);
    easu.con[0][3] = as_uint(0.5f * sourceHeight / outputHeight - 0.5f);
    easu.con[1][0] = as_uint(1.0f / sourceWidth);
    easu.con[1][1] = as_uint(1.0f / sourceHeight);
    easu.con[1][2] = as_uint(1.0f / sourceWidth);
    easu.con[1][3] = as_uint(-1.0f / sourceHeight);
    easu.con[2][0] = as_uint(-1.0f / sourceWidth);
    easu.con[2][1] = as_uint(2.0f / sourceHeight);
    easu.con[2][2] = as_uint(1.0f / sourceWidth);
    easu.con[2][3] = as_uint(2.0f / sourceHeight);
    easu.con[3][0] = 0;
    easu.con[3][1] = as_uint(4.0f / sourceHeight);
    easu.con[3][2] = 0;
    easu.con[3][3] = 0;

    context_->UpdateSubresource(easuConstants_.Get(), 0, nullptr, &easu, 0, 0);
    hr = draw_pass(source, intermediateRtv_.Get(), easuPixelShader_.Get(),
                   destinationDesc.Width, destinationDesc.Height, easuConstants_.Get());
    if (FAILED(hr)) return hr;

    const float stops = std::clamp(rcasSharpnessStops, 0.0f, 2.0f);
    RcasConstants rcas{};
    rcas.con[0] = as_uint(std::exp2(-stops));
    context_->UpdateSubresource(rcasConstants_.Get(), 0, nullptr, &rcas, 0, 0);

    return draw_pass(intermediateSrv_.Get(), destination, rcasPixelShader_.Get(),
                     destinationDesc.Width, destinationDesc.Height, rcasConstants_.Get());
}

void D3D11Scaler::reset() noexcept {
    initialized_ = false;
    intermediateSrv_.Reset();
    intermediateRtv_.Reset();
    intermediateTexture_.Reset();
    rcasConstants_.Reset();
    easuConstants_.Reset();
    depthDisabled_.Reset();
    blend_.Reset();
    rasterizer_.Reset();
    sampler_.Reset();
    rcasPixelShader_.Reset();
    easuPixelShader_.Reset();
    linearPixelShader_.Reset();
    vertexShader_.Reset();
    context_.Reset();
    device_.Reset();
}

} // namespace end_scaling
