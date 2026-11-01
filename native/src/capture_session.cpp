#include "end_scaling/capture_session.hpp"
#include "end_scaling/d3d11_scaler.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

namespace end_scaling {
namespace {

constexpr UINT kBufferCount = 2;
constexpr DXGI_FORMAT kPixelFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::SizeInt32;

struct ApartmentScope {
    ApartmentScope() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~ApartmentScope() { winrt::uninit_apartment(); }
};

std::wstring hr_text(const wchar_t* action, HRESULT hr) {
    wchar_t buffer[256]{};
    swprintf_s(buffer, L"%s failed (HRESULT 0x%08X).", action, static_cast<unsigned>(hr));
    return buffer;
}

class WindowCapture final : public std::enable_shared_from_this<WindowCapture> {
public:
    ~WindowCapture() { stop_noexcept(); }

    HRESULT start(HWND source, HWND output, ScaleMode mode,
                  const wchar_t* shaderPath, std::wstring* error) {
        try {
            ApartmentScope apartment;
            if (!GraphicsCaptureSession::IsSupported()) {
                if (error) *error = L"Windows.Graphics.Capture is not available on this PC.";
                return E_NOTIMPL;
            }
            if (!IsWindow(source) || !IsWindow(output) || source == output ||
                shaderPath == nullptr || shaderPath[0] == L'\0') {
                if (error) *error = L"Invalid source window, preview window, or shader path.";
                return E_INVALIDARG;
            }

            source_ = source;
            output_ = output;
            mode_ = mode;

            HRESULT hr = create_d3d_device();
            if (FAILED(hr)) {
                if (error) *error = hr_text(L"Hardware Direct3D 11 device creation", hr);
                return hr;
            }

            hr = scaler_.initialize(device_.Get(), shaderPath, error);
            if (FAILED(hr)) return hr;

            hr = create_swap_chain();
            if (FAILED(hr)) {
                if (error) *error = hr_text(L"Preview swap-chain creation", hr);
                return hr;
            }

            auto factory = winrt::get_activation_factory<
                GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(factory->CreateForWindow(
                source_, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_)));

            size_ = item_.Size();
            if (size_.Width <= 0 || size_.Height <= 0) {
                if (error) *error = L"The selected window has no drawable content.";
                return E_INVALIDARG;
            }

            winrtDevice_ = make_winrt_device();
            framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized,
                static_cast<int>(kBufferCount), size_);
            session_ = framePool_.CreateCaptureSession(item_);
            session_.IsCursorCaptureEnabled(true);

            std::weak_ptr<WindowCapture> weak = shared_from_this();
            token_ = framePool_.FrameArrived(
                [weak](Direct3D11CaptureFramePool const& sender,
                       winrt::Windows::Foundation::IInspectable const&) {
                    if (auto self = weak.lock()) self->on_frame(sender);
                });
            hasHandler_ = true;

            running_ = true;
            session_.StartCapture();
            return S_OK;
        }
        catch (const winrt::hresult_error& exception) {
            if (error) *error = exception.message().c_str();
            stop_noexcept();
            return exception.code();
        }
        catch (...) {
            if (error) *error = L"Unexpected Windows Graphics Capture initialization error.";
            stop_noexcept();
            return E_FAIL;
        }
    }

    void stop() noexcept {
        try {
            ApartmentScope apartment;
            std::lock_guard<std::mutex> guard(mutex_);
            stop_locked();
        }
        catch (...) {
            stop_noexcept();
        }
    }

private:
    HRESULT create_d3d_device() {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, &device_, &selected, &context_);
        if (FAILED(hr)) return hr;

        // All immediate-context work is serialized by mutex_; avoid the extra
        // per-call overhead of enabling D3D11's multithread protection layer.
        return S_OK;
    }

    IDirect3DDevice make_winrt_device() {
        ComPtr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(device_.As(&dxgiDevice));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.Get(), reinterpret_cast<IInspectable**>(inspectable.put())));
        return inspectable.as<IDirect3DDevice>();
    }

    HRESULT create_swap_chain() {
        RECT rc{};
        if (!GetClientRect(output_, &rc)) return HRESULT_FROM_WIN32(GetLastError());
        const UINT width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = device_.As(&dxgiDevice);
        if (FAILED(hr)) return hr;
        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr)) return hr;
        ComPtr<IDXGIFactory2> factory;
        hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = kPixelFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        hr = factory->CreateSwapChainForHwnd(
            device_.Get(), output_, &desc, nullptr, nullptr, &swapChain_);
        if (FAILED(hr)) return hr;
        factory->MakeWindowAssociation(output_, DXGI_MWA_NO_ALT_ENTER);
        return create_back_buffer();
    }

    HRESULT create_back_buffer() {
        backBuffer_.Reset();
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&texture));
        if (FAILED(hr)) return hr;
        return device_->CreateRenderTargetView(texture.Get(), nullptr, &backBuffer_);
    }

    HRESULT resize_output() {
        if (!IsWindow(output_) || !swapChain_) return E_HANDLE;
        RECT rc{};
        if (!GetClientRect(output_, &rc)) return HRESULT_FROM_WIN32(GetLastError());
        const UINT width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

        DXGI_SWAP_CHAIN_DESC1 current{};
        HRESULT hr = swapChain_->GetDesc1(&current);
        if (FAILED(hr)) return hr;
        if (current.Width == width && current.Height == height) return S_OK;

        backBuffer_.Reset();
        hr = swapChain_->ResizeBuffers(2, width, height, kPixelFormat, 0);
        if (FAILED(hr)) return hr;
        return create_back_buffer();
    }

    HRESULT get_source_view(ID3D11Texture2D* texture,
                            ID3D11ShaderResourceView** result) {
        for (std::size_t i = 0; i < textureCache_.size(); ++i) {
            if (textureCache_[i].Get() == texture && viewCache_[i]) {
                *result = viewCache_[i].Get();
                (*result)->AddRef();
                return S_OK;
            }
        }

        const auto slot = cacheCursor_++ % textureCache_.size();
        textureCache_[slot] = texture;
        viewCache_[slot].Reset();
        HRESULT hr = device_->CreateShaderResourceView(texture, nullptr, &viewCache_[slot]);
        if (FAILED(hr)) {
            textureCache_[slot].Reset();
            return hr;
        }
        *result = viewCache_[slot].Get();
        (*result)->AddRef();
        return S_OK;
    }

    void on_frame(Direct3D11CaptureFramePool const& sender) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_) return;
        try {
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            // Do not build a latency queue. Consume any waiting frames and render
            // only the freshest frame available in the two-buffer pool.
            for (;;) {
                auto newer = sender.TryGetNextFrame();
                if (!newer) break;
                frame = std::move(newer);
            }

            const SizeInt32 content = frame.ContentSize();
            if (content.Width <= 0 || content.Height <= 0) return;
            if (content.Width != size_.Width || content.Height != size_.Height) {
                size_ = content;
                frame = nullptr;
                for (auto& view : viewCache_) view.Reset();
                for (auto& texture : textureCache_) texture.Reset();
                cacheCursor_ = 0;
                framePool_.Recreate(winrtDevice_,
                    DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    static_cast<int>(kBufferCount), size_);
                return;
            }

            auto dxgiAccess = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> frameTexture;
            HRESULT hr = dxgiAccess->GetInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(frameTexture.GetAddressOf()));
            if (FAILED(hr)) return;

            ComPtr<ID3D11ShaderResourceView> sourceView;
            if (FAILED(get_source_view(frameTexture.Get(), &sourceView))) return;
            if (FAILED(resize_output())) return;

            hr = scaler_.draw(sourceView.Get(), backBuffer_.Get(), mode_,
                mode_ == ScaleMode::quality ? 0.0f : 1.5f);
            if (FAILED(hr)) return;

            const HRESULT present = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
            if (present == DXGI_ERROR_DEVICE_REMOVED || present == DXGI_ERROR_DEVICE_RESET) {
                running_ = false;
            }
        }
        catch (...) {
            // Never unwind C++ exceptions through a WinRT event callback.
        }
    }

    void stop_noexcept() noexcept {
        try {
            ApartmentScope apartment;
            std::lock_guard<std::mutex> guard(mutex_);
            stop_locked();
        }
        catch (...) {
        }
    }

    void stop_locked() noexcept {
        running_ = false;
        try {
            if (framePool_ && hasHandler_) {
                framePool_.FrameArrived(token_);
                hasHandler_ = false;
            }
            if (session_) session_.Close();
            if (framePool_) framePool_.Close();
        }
        catch (...) {
        }

        session_ = nullptr;
        framePool_ = nullptr;
        item_ = nullptr;
        winrtDevice_ = nullptr;
        for (auto& view : viewCache_) view.Reset();
        for (auto& texture : textureCache_) texture.Reset();
        backBuffer_.Reset();
        swapChain_.Reset();
        scaler_.reset();
        context_.Reset();
        device_.Reset();
    }

    HWND source_ = nullptr;
    HWND output_ = nullptr;
    ScaleMode mode_ = ScaleMode::balanced;
    bool running_ = false;
    bool hasHandler_ = false;
    std::mutex mutex_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3D11Scaler scaler_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> backBuffer_;
    std::array<ComPtr<ID3D11Texture2D>, 2> textureCache_{};
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> viewCache_{};
    std::size_t cacheCursor_ = 0;

    IDirect3DDevice winrtDevice_{nullptr};
    GraphicsCaptureItem item_{nullptr};
    Direct3D11CaptureFramePool framePool_{nullptr};
    GraphicsCaptureSession session_{nullptr};
    winrt::event_token token_{};
    SizeInt32 size_{};
};

} // namespace

void* start_window_capture(HWND source, HWND output, std::uint32_t mode,
                           const wchar_t* shaderPath, std::wstring* error) noexcept {
    try {
        auto capture = std::make_shared<WindowCapture>();
        const ScaleMode selected = mode == 0 ? ScaleMode::quality :
                                  mode == 2 ? ScaleMode::performance : ScaleMode::balanced;
        HRESULT hr = capture->start(source, output, selected, shaderPath, error);
        if (FAILED(hr)) return nullptr;
        return new std::shared_ptr<WindowCapture>(std::move(capture));
    }
    catch (...) {
        if (error) *error = L"Could not allocate capture resources.";
        return nullptr;
    }
}

void stop_window_capture(void* handle) noexcept {
    if (!handle) return;
    auto* capture = static_cast<std::shared_ptr<WindowCapture>*>(handle);
    (*capture)->stop();
    delete capture;
}

} // namespace end_scaling

extern "C" __declspec(dllexport) void* __cdecl es_capture_start(
    HWND source, HWND output, std::uint32_t mode, const wchar_t* shaderPath,
    wchar_t* errorText, int errorCapacity) noexcept {
    std::wstring error;
    void* handle = end_scaling::start_window_capture(source, output, mode, shaderPath, &error);
    if (!handle && errorText && errorCapacity > 0) {
        wcsncpy_s(errorText, static_cast<std::size_t>(errorCapacity), error.c_str(), _TRUNCATE);
    }
    return handle;
}

extern "C" __declspec(dllexport) void __cdecl es_capture_stop(void* handle) noexcept {
    end_scaling::stop_window_capture(handle);
}
