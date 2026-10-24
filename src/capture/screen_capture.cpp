#include "dogsc/screen_capture.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

namespace dogsc::capture {
namespace capture = winrt::Windows::Graphics::Capture;
namespace directx = winrt::Windows::Graphics::DirectX;

std::int64_t host_time_100ns() {
    static const auto frequency = [] {
        LARGE_INTEGER value{};
        winrt::check_bool(QueryPerformanceFrequency(&value));
        return value.QuadPart;
    }();
    LARGE_INTEGER value{};
    winrt::check_bool(QueryPerformanceCounter(&value));
    return (value.QuadPart / frequency) * 10000000 +
           (value.QuadPart % frequency) * 10000000 / frequency;
}

struct ScreenCapture::Impl {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device{nullptr};
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::com_ptr<ID3D11RenderTargetView> target_view;
    winrt::Windows::Graphics::SizeInt32 pool_size{};
    RECT region{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::shared_ptr<std::atomic_bool> is_closed = std::make_shared<std::atomic_bool>(false);
    bool running = false;

    explicit Impl(const CaptureTarget& target) {
        if (!capture::GraphicsCaptureSession::IsSupported()) {
            throw std::runtime_error("Windows.Graphics.Capture 不受支持");
        }
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
        auto multithread = context.as<ID3D11Multithread>();
        multithread->SetMultithreadProtected(TRUE);
        auto dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        winrt_device = inspectable.as<decltype(winrt_device)>();
        auto factory = winrt::get_activation_factory<capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        if (target.window) {
            winrt::check_hresult(factory->CreateForWindow(target.window,
                winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        } else if (target.monitor) {
            winrt::check_hresult(factory->CreateForMonitor(target.monitor,
                winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
        } else {
            throw std::invalid_argument("未指定采集项");
        }
        const auto state = is_closed;
        item.Closed([state](auto&&, auto&&) { state->store(true); });
        pool_size = item.Size();
        region = target.region;
        if (region.right == 0 && region.bottom == 0) {
            region = {0, 0, pool_size.Width, pool_size.Height};
        }
        if (region.left < 0 || region.top < 0 || region.right > pool_size.Width ||
            region.bottom > pool_size.Height || region.right <= region.left ||
            region.bottom <= region.top) {
            throw std::invalid_argument("采集区域超出采集项边界");
        }
        width = static_cast<std::uint32_t>(region.right - region.left) & ~1U;
        height = static_cast<std::uint32_t>(region.bottom - region.top) & ~1U;
        if (width == 0 || height == 0) throw std::invalid_argument("采集区域小于 2 像素");
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        winrt::check_hresult(device->CreateTexture2D(&description, nullptr, texture.put()));
        winrt::check_hresult(device->CreateRenderTargetView(texture.get(), nullptr,
            target_view.put()));
        pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(winrt_device,
            directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, pool_size);
        session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(target.include_cursor);
    }
};

ScreenCapture::ScreenCapture(const CaptureTarget& target) : impl_(std::make_unique<Impl>(target)) {}
ScreenCapture::~ScreenCapture() {
    try { stop(); } catch (...) {}
}
ID3D11Device* ScreenCapture::device() const { return impl_->device.get(); }
std::uint32_t ScreenCapture::width() const { return impl_->width; }
std::uint32_t ScreenCapture::height() const { return impl_->height; }
bool ScreenCapture::closed() const { return impl_->is_closed->load(); }
void ScreenCapture::start() {
    if (!impl_->running) {
        impl_->session.StartCapture();
        impl_->running = true;
    }
}
void ScreenCapture::stop() {
    if (impl_->session) impl_->session.Close();
    if (impl_->pool) impl_->pool.Close();
    impl_->session = nullptr;
    impl_->pool = nullptr;
    impl_->running = false;
}
ScreenFrame ScreenCapture::poll() {
    auto& state = *impl_;
    if (!state.running) return {};
    auto frame = state.pool.TryGetNextFrame();
    if (!frame) return {};
    // 丢弃积压帧，只消费最新的一帧，避免编码背压变成无限延迟。
    while (auto next = state.pool.TryGetNextFrame()) {
        frame.Close();
        frame = std::move(next);
    }
    auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> source;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), source.put_void()));
    D3D11_TEXTURE2D_DESC source_description{};
    source->GetDesc(&source_description);
    const auto content = frame.ContentSize();
    const auto available_width = std::min(source_description.Width,
        static_cast<UINT>(std::max(content.Width, 0)));
    const auto available_height = std::min(source_description.Height,
        static_cast<UINT>(std::max(content.Height, 0)));
    const float black[] = {0, 0, 0, 1};
    state.context->ClearRenderTargetView(state.target_view.get(), black);
    const auto left = static_cast<UINT>(state.region.left);
    const auto top = static_cast<UINT>(state.region.top);
    if (left < available_width && top < available_height) {
        D3D11_BOX box{left, top, 0, std::min(left + state.width, available_width),
            std::min(top + state.height, available_height), 1};
        state.context->CopySubresourceRegion(state.texture.get(), 0, 0, 0, 0, source.get(), 0, &box);
    }
    const auto timestamp = frame.SystemRelativeTime().count();
    frame.Close();
    // 调整 frame pool 尺寸，编码画布仍固定；缩小后的空白区域明确填黑。
    if (content.Width > 0 && content.Height > 0 &&
        (content.Width != state.pool_size.Width || content.Height != state.pool_size.Height)) {
        state.pool_size = content;
        state.pool.Recreate(state.winrt_device, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            3, state.pool_size);
    }
    return {state.texture, timestamp};
}
} // namespace dogsc::capture
