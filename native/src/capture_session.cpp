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
#include <atomic>
#include <chrono>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>

using Microsoft::WRL::ComPtr;

extern "C" void __cdecl es_crash_set_context(
    std::uint32_t stage, std::int32_t detail) noexcept;

namespace {
void crash_stage(std::uint32_t stage, HRESULT detail = S_OK) noexcept {
    es_crash_set_context(stage, static_cast<std::int32_t>(detail));
}
}

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
            crash_stage(10); // capture start entered
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
            mode_.store(mode, std::memory_order_relaxed);

            crash_stage(11); // create D3D11 device
            HRESULT hr = create_d3d_device();
            if (FAILED(hr)) {
                if (error) *error = hr_text(L"Hardware Direct3D 11 device creation", hr);
                return hr;
            }

            crash_stage(12); // create GPU timestamp queries
            hr = create_gpu_timing_queries();
            if (FAILED(hr)) {
                if (error) *error = hr_text(L"GPU timing query creation", hr);
                return hr;
            }

            crash_stage(13); // initialize FSR shaders and resources
            hr = scaler_.initialize(device_.Get(), shaderPath, error);
            if (FAILED(hr)) return hr;

            crash_stage(14); // create output swap chain
            hr = create_swap_chain();
            if (FAILED(hr)) {
                if (error) *error = hr_text(L"Preview swap-chain creation", hr);
                return hr;
            }

            crash_stage(15); // create GraphicsCaptureItem for source HWND
            auto factory = winrt::get_activation_factory<
                GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::check_hresult(factory->CreateForWindow(
                source_, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item_)));

            size_ = item_.Size();
            if (size_.Width <= 0 || size_.Height <= 0) {
                if (error) *error = L"The selected window has no drawable content.";
                return E_INVALIDARG;
            }

            crash_stage(16); // wrap D3D device for WinRT capture
            winrtDevice_ = make_winrt_device();
            framePool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice_, DirectXPixelFormat::B8G8R8A8UIntNormalized,
                static_cast<int>(kBufferCount), size_);
            crash_stage(17); // create capture session
            session_ = framePool_.CreateCaptureSession(item_);
            // The desktop cursor is drawn above the output HWND. Capturing it
            // into the source frame as well would show a duplicated cursor.
            session_.IsCursorCaptureEnabled(false);

            std::weak_ptr<WindowCapture> weak = shared_from_this();
            token_ = framePool_.FrameArrived(
                [weak](Direct3D11CaptureFramePool const& sender,
                       winrt::Windows::Foundation::IInspectable const&) {
                    if (auto self = weak.lock()) self->on_frame(sender);
                });
            hasHandler_ = true;

            renderedFrames_.store(0, std::memory_order_relaxed);
            droppedFrames_.store(0, std::memory_order_relaxed);
            inactiveFrames_.store(0, std::memory_order_relaxed);
            outputFps_.store(0.0, std::memory_order_relaxed);
            lastSubmitMs_.store(0.0, std::memory_order_relaxed);
            lastGpuMs_.store(0.0, std::memory_order_relaxed);
            lastError_.store(S_OK, std::memory_order_relaxed);
            fpsWindowStartedAt_ = std::chrono::steady_clock::now();
            fpsWindowFrames_ = 0;
            wasOutputInactive_ = false;
            running_ = true;
            crash_stage(18); // start capture
            session_.StartCapture();
            crash_stage(19); // capture started
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

    void set_mode(ScaleMode mode) noexcept {
        mode_.store(mode, std::memory_order_relaxed);
    }

    void get_stats(double* fps, double* lastSubmitMs, double* gpuMs,
                   std::uint64_t* frames, std::uint64_t* dropped,
                   std::uint64_t* inactive) const noexcept {
        if (fps) *fps = outputFps_.load(std::memory_order_relaxed);
        if (lastSubmitMs) *lastSubmitMs = lastSubmitMs_.load(std::memory_order_relaxed);
        if (gpuMs) *gpuMs = lastGpuMs_.load(std::memory_order_relaxed);
        if (frames) *frames = renderedFrames_.load(std::memory_order_relaxed);
        if (dropped) *dropped = droppedFrames_.load(std::memory_order_relaxed);
        if (inactive) *inactive = inactiveFrames_.load(std::memory_order_relaxed);
    }

    void get_status(int* running, std::int32_t* lastError) const noexcept {
        if (running) *running = running_.load(std::memory_order_relaxed) ? 1 : 0;
        if (lastError) *lastError = lastError_.load(std::memory_order_relaxed);
    }

private:
    struct GpuTimingSlot {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> start;
        ComPtr<ID3D11Query> end;
        bool pending = false;
    };

    HRESULT create_gpu_timing_queries() {
        D3D11_QUERY_DESC desc{};
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        for (auto& slot : gpuTimings_) {
            HRESULT hr = device_->CreateQuery(&desc, &slot.disjoint);
            if (FAILED(hr)) return hr;
            desc.Query = D3D11_QUERY_TIMESTAMP;
            hr = device_->CreateQuery(&desc, &slot.start);
            if (FAILED(hr)) return hr;
            hr = device_->CreateQuery(&desc, &slot.end);
            if (FAILED(hr)) return hr;
            desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        }
        return S_OK;
    }

    // Read only completed queries. DONOTFLUSH avoids forcing the GPU to submit
    // work just to update diagnostics; a busy slot is simply checked next frame.
    void collect_gpu_timings() noexcept {
        for (auto& slot : gpuTimings_) {
            if (!slot.pending) continue;

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 begin = 0;
            UINT64 end = 0;
            const HRESULT disjointHr = context_->GetData(
                slot.disjoint.Get(), &disjoint, sizeof(disjoint),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT beginHr = context_->GetData(
                slot.start.Get(), &begin, sizeof(begin),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            const HRESULT endHr = context_->GetData(
                slot.end.Get(), &end, sizeof(end),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);

            if (disjointHr == S_FALSE || beginHr == S_FALSE || endHr == S_FALSE) {
                continue;
            }

            slot.pending = false;
            if (SUCCEEDED(disjointHr) && SUCCEEDED(beginHr) && SUCCEEDED(endHr) &&
                !disjoint.Disjoint && disjoint.Frequency != 0 && end >= begin) {
                const double milliseconds =
                    static_cast<double>(end - begin) * 1000.0 /
                    static_cast<double>(disjoint.Frequency);
                lastGpuMs_.store(milliseconds, std::memory_order_relaxed);
            }
        }
    }

    GpuTimingSlot* begin_gpu_timing() noexcept {
        for (std::size_t offset = 0; offset < gpuTimings_.size(); ++offset) {
            const std::size_t index = (gpuTimingCursor_ + offset) % gpuTimings_.size();
            auto& slot = gpuTimings_[index];
            if (slot.pending) continue;
            context_->Begin(slot.disjoint.Get());
            context_->End(slot.start.Get());
            return &slot;
        }
        return nullptr;
    }

    void end_gpu_timing(GpuTimingSlot* slot) noexcept {
        if (!slot) return;
        context_->End(slot->end.Get());
        context_->End(slot->disjoint.Get());
        slot->pending = true;
        gpuTimingCursor_ =
            (static_cast<std::size_t>(slot - gpuTimings_.data()) + 1) % gpuTimings_.size();
    }

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

        // The immediate context keeps references to bound render targets.
        // Release the back-buffer binding before ResizeBuffers or DXGI may
        // reject the resize with DXGI_ERROR_INVALID_CALL.
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        backBuffer_.Reset();
        hr = swapChain_->ResizeBuffers(2, width, height, kPixelFormat, 0);
        if (FAILED(hr)) return hr;
        return create_back_buffer();
    }


    void record_frame_failure(HRESULT hr) noexcept {
        crash_stage(195, hr); // a frame operation returned a failed HRESULT
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
        lastError_.store(static_cast<std::int32_t>(hr), std::memory_order_relaxed);

        const HRESULT removedReason = device_ ? device_->GetDeviceRemovedReason() : S_OK;
        if (hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_RESET ||
            hr == DXGI_ERROR_DEVICE_HUNG ||
            FAILED(removedReason)) {
            // A removed device cannot recover by continuing to submit frames.
            // Keep the HRESULT for the UI and stop the callback path cleanly.
            const HRESULT reason = FAILED(removedReason) ? removedReason : hr;
            lastError_.store(static_cast<std::int32_t>(reason), std::memory_order_relaxed);
            running_.store(false, std::memory_order_relaxed);
        }
    }

    void on_frame(Direct3D11CaptureFramePool const& sender) noexcept {
        crash_stage(100); // FrameArrived callback entered
        std::lock_guard<std::mutex> guard(mutex_);
        if (!running_) return;
        try {
            collect_gpu_timings();
            crash_stage(101); // acquire newest capture frame
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            // Do not build a latency queue. Consume any waiting frames and render
            // only the freshest frame available in the two-buffer pool.
            crash_stage(102); // drain queued stale frames
            for (;;) {
                auto newer = sender.TryGetNextFrame();
                if (!newer) break;
                frame = std::move(newer);
            }

            // When the output is hidden (for example after Alt+Tab), release
            // the newest capture frame without running the scaler or Present.
            // This keeps the inactive overlay from consuming avoidable GPU time.
            if (!IsWindowVisible(output_) || IsIconic(output_)) {
                inactiveFrames_.fetch_add(1, std::memory_order_relaxed);
                outputFps_.store(0.0, std::memory_order_relaxed);
                fpsWindowStartedAt_ = std::chrono::steady_clock::now();
                fpsWindowFrames_ = 0;
                wasOutputInactive_ = true;
                return;
            }

            const auto frameNow = std::chrono::steady_clock::now();
            if (wasOutputInactive_) {
                fpsWindowStartedAt_ = frameNow;
                fpsWindowFrames_ = 0;
                wasOutputInactive_ = false;
            }

            crash_stage(103); // inspect frame dimensions
            const SizeInt32 content = frame.ContentSize();
            if (content.Width <= 0 || content.Height <= 0) return;
            if (content.Width != size_.Width || content.Height != size_.Height) {
                size_ = content;
                frame = nullptr;
                framePool_.Recreate(winrtDevice_,
                    DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    static_cast<int>(kBufferCount), size_);
                return;
            }

            crash_stage(104); // obtain the D3D interface for the frame surface
            auto dxgiAccess = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> frameTexture;
            HRESULT hr = dxgiAccess->GetInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(frameTexture.GetAddressOf()));
            if (FAILED(hr)) {
                record_frame_failure(hr);
                return;
            }

            ComPtr<ID3D11ShaderResourceView> sourceView;
            // The WGC surface is only used while its frame is checked out.
            // Do not keep SRVs to frame-pool textures after returning a frame.
            crash_stage(105); // create source shader-resource view
            hr = device_->CreateShaderResourceView(frameTexture.Get(), nullptr, &sourceView);
            if (FAILED(hr)) {
                record_frame_failure(hr);
                return;
            }
            crash_stage(106); // resize swap-chain buffers when needed
            hr = resize_output();
            if (FAILED(hr)) {
                record_frame_failure(hr);
                return;
            }

            GpuTimingSlot* gpuTiming = begin_gpu_timing();
            const ScaleMode activeMode = mode_.load(std::memory_order_relaxed);
            const auto submitStart = std::chrono::steady_clock::now();
            crash_stage(107); // submit scaling passes to D3D11
            hr = scaler_.draw(sourceView.Get(), backBuffer_.Get(), activeMode,
                activeMode == ScaleMode::quality ? 0.0f : 1.5f);
            end_gpu_timing(gpuTiming);
            if (FAILED(hr)) {
                record_frame_failure(hr);
                return;
            }

            crash_stage(108); // present scaled frame
            const HRESULT present = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
            if (present == DXGI_ERROR_WAS_STILL_DRAWING) {
                droppedFrames_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (FAILED(present)) {
                record_frame_failure(present);
                return;
            }

            const auto presentedAt = std::chrono::steady_clock::now();
            const double elapsedMs = std::chrono::duration<double, std::milli>(
                presentedAt - submitStart).count();
            lastSubmitMs_.store(elapsedMs, std::memory_order_relaxed);
            renderedFrames_.fetch_add(1, std::memory_order_relaxed);

            crash_stage(109); // frame presented successfully
            ++fpsWindowFrames_;
            const double fpsWindowSeconds = std::chrono::duration<double>(
                presentedAt - fpsWindowStartedAt_).count();
            if (fpsWindowSeconds >= 0.5) {
                outputFps_.store(
                    static_cast<double>(fpsWindowFrames_) / fpsWindowSeconds,
                    std::memory_order_relaxed);
                fpsWindowStartedAt_ = presentedAt;
                fpsWindowFrames_ = 0;
            }
        }
        catch (...) {
            crash_stage(190, E_FAIL); // C++ exception was caught inside FrameArrived
            // Never unwind C++ exceptions through a WinRT event callback.
            // Also check whether the D3D device was removed during the failure.
            record_frame_failure(E_FAIL);
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
        crash_stage(200); // capture shutdown entered
        running_ = false;
        try {
            crash_stage(201); // remove FrameArrived handler
            if (framePool_ && hasHandler_) {
                framePool_.FrameArrived(token_);
                hasHandler_ = false;
            }
            crash_stage(202); // close capture session
            if (session_) session_.Close();
            crash_stage(203); // close frame pool
            if (framePool_) framePool_.Close();
        }
        catch (...) {
        }

        crash_stage(204); // release COM and D3D resources
        session_ = nullptr;
        framePool_ = nullptr;
        item_ = nullptr;
        winrtDevice_ = nullptr;
        backBuffer_.Reset();
        swapChain_.Reset();
        scaler_.reset();
        context_.Reset();
        device_.Reset();
    }

    HWND source_ = nullptr;
    HWND output_ = nullptr;
    std::atomic<ScaleMode> mode_{ScaleMode::balanced};
    std::atomic<bool> running_{false};
    std::atomic<std::int32_t> lastError_{S_OK};
    bool hasHandler_ = false;
    std::mutex mutex_;
    std::chrono::steady_clock::time_point fpsWindowStartedAt_{};
    std::uint64_t fpsWindowFrames_ = 0; // guarded by mutex_
    bool wasOutputInactive_ = false;    // guarded by mutex_
    std::atomic<std::uint64_t> renderedFrames_{0};
    std::atomic<std::uint64_t> droppedFrames_{0};
    std::atomic<std::uint64_t> inactiveFrames_{0};
    std::atomic<double> outputFps_{0.0};
    std::atomic<double> lastSubmitMs_{0.0};
    std::atomic<double> lastGpuMs_{0.0};
    std::array<GpuTimingSlot, 4> gpuTimings_{};
    std::size_t gpuTimingCursor_ = 0;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    D3D11Scaler scaler_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> backBuffer_;
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

void set_window_capture_mode(void* handle, std::uint32_t mode) noexcept {
    if (!handle) return;
    const ScaleMode selected = mode == 0 ? ScaleMode::quality :
                               mode == 2 ? ScaleMode::performance : ScaleMode::balanced;
    auto* capture = static_cast<std::shared_ptr<WindowCapture>*>(handle);
    (*capture)->set_mode(selected);
}

void get_window_capture_stats(void* handle, double* fps,
                               double* lastSubmitMs, double* gpuMs,
                               std::uint64_t* frames, std::uint64_t* dropped,
                               std::uint64_t* inactive) noexcept {
    if (fps) *fps = 0.0;
    if (lastSubmitMs) *lastSubmitMs = 0.0;
    if (gpuMs) *gpuMs = 0.0;
    if (frames) *frames = 0;
    if (dropped) *dropped = 0;
    if (inactive) *inactive = 0;
    if (!handle) return;
    auto* capture = static_cast<std::shared_ptr<WindowCapture>*>(handle);
    (*capture)->get_stats(fps, lastSubmitMs, gpuMs, frames, dropped, inactive);
}

void get_window_capture_status(void* handle, int* running,
                               std::int32_t* lastError) noexcept {
    if (running) *running = 0;
    if (lastError) *lastError = E_HANDLE;
    if (!handle) return;
    auto* capture = static_cast<std::shared_ptr<WindowCapture>*>(handle);
    (*capture)->get_status(running, lastError);
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

extern "C" __declspec(dllexport) void __cdecl es_capture_set_mode(
    void* handle, std::uint32_t mode) noexcept {
    end_scaling::set_window_capture_mode(handle, mode);
}

extern "C" __declspec(dllexport) int __cdecl es_capture_get_stats(
    void* handle, double* fps, double* lastSubmitMs, double* gpuMs,
    std::uint64_t* frames, std::uint64_t* dropped,
    std::uint64_t* inactive) noexcept {
    end_scaling::get_window_capture_stats(
        handle, fps, lastSubmitMs, gpuMs, frames, dropped, inactive);
    return handle != nullptr ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl es_capture_get_status(
    void* handle, int* running, std::int32_t* lastError) noexcept {
    end_scaling::get_window_capture_status(handle, running, lastError);
    return handle != nullptr ? 1 : 0;
}

extern "C" __declspec(dllexport) void __cdecl es_capture_stop(void* handle) noexcept {
    end_scaling::stop_window_capture(handle);
}
