#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace end_scaling {

enum class ScaleMode : std::uint32_t {
    performance = 0, // One bilinear pass: lowest GPU cost.
    balanced = 1,    // AMD FSR 1 EASU + RCAS.
    quality = 2      // AMD FSR 1 EASU + stronger RCAS.
};

// Reusable D3D11 scaling pipeline. The caller owns the D3D device and must
// serialize access to its immediate context while draw() is running.
class D3D11Scaler final {
public:
    D3D11Scaler() = default;
    ~D3D11Scaler() = default;
    D3D11Scaler(const D3D11Scaler&) = delete;
    D3D11Scaler& operator=(const D3D11Scaler&) = delete;

    HRESULT initialize(ID3D11Device* device, const wchar_t* shaderPath,
                       std::wstring* error = nullptr);
    HRESULT draw(ID3D11ShaderResourceView* source,
                 ID3D11RenderTargetView* destination,
                 ScaleMode mode, float rcasSharpnessStops = 1.5f);
    void reset() noexcept;

private:
    HRESULT create_states();
    HRESULT create_constant_buffers();
    HRESULT ensure_intermediate(UINT width, UINT height);
    HRESULT draw_pass(ID3D11ShaderResourceView* source,
                      ID3D11RenderTargetView* destination,
                      ID3D11PixelShader* pixelShader,
                      UINT width, UINT height, ID3D11Buffer* constants);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> linearPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> easuPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> rcasPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthDisabled_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> easuConstants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> rcasConstants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> intermediateTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> intermediateRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> intermediateSrv_;
    bool initialized_ = false;
};

} // namespace end_scaling
