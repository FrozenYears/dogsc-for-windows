#include "dxgi_preview_overlay.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <winrt/base.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace dogsc::cli {
namespace {
using winrt::com_ptr;
constexpr wchar_t kPreviewWindowClass[] = L"DogSC.NativePreviewOverlay";

void check(HRESULT result, const char* stage) {
    if (FAILED(result)) {
        char code[16]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(stage) + ": " + code);
    }
}
}  // namespace

struct DxgiPreviewOverlay::Impl {
    HWND owner = nullptr;
    HWND window = nullptr;
    HINSTANCE instance = nullptr;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGIFactory2> factory;
    com_ptr<IDXGISwapChain1> swapchain;
    UINT buffer_width = 0;
    UINT buffer_height = 0;
    bool visible = false;
    std::uint64_t presented = 0;
    std::uint64_t dropped = 0;

    explicit Impl(std::uintptr_t owner_handle) : owner(reinterpret_cast<HWND>(owner_handle)) {
        if (!owner || !IsWindow(owner)) throw std::invalid_argument("preview owner HWND 无效");
        instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kPreviewWindowClass;
        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("注册 DXGI preview window class 失败");
        }
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPreviewWindowClass, L"",
            WS_POPUP, 0, 0, 1, 1, owner, nullptr, instance, this);
        if (!window) throw std::runtime_error("创建 DXGI preview window 失败");
        try {
            const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()),
                "Create preview D3D11 device");
            check(CreateDXGIFactory1(__uuidof(IDXGIFactory2), factory.put_void()), "Create preview DXGI factory");
        } catch (...) {
            DestroyWindow(window);
            window = nullptr;
            throw;
        }
    }

    ~Impl() {
        swapchain = nullptr;
        if (window) DestroyWindow(window);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(creation->lpCreateParams));
        }
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND) return 1;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void pump_messages() {
        MSG message{};
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void set_bounds(int x, int y, int width, int height, bool show) {
        pump_messages();
        if (!show || width <= 0 || height <= 0 || !IsWindow(owner) || IsIconic(owner)) {
            visible = false;
            ShowWindow(window, SW_HIDE);
            return;
        }
        if (!SetWindowPos(window, HWND_TOP, x, y, width, height,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
            throw std::runtime_error("定位 DXGI preview window 失败");
        }
        visible = true;
    }

    void create_swapchain(UINT width, UINT height) {
        swapchain = nullptr;
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        check(factory->CreateSwapChainForHwnd(device.get(), window, &description, nullptr, nullptr,
            swapchain.put()), "Create preview DXGI swapchain");
        buffer_width = width;
        buffer_height = height;
    }

    bool present(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t height) {
        pump_messages();
        const auto expected = static_cast<std::size_t>(width) * height * 4;
        if (width == 0 || height == 0 || pixels.size() != expected) {
            throw std::invalid_argument("DXGI preview BGRA buffer size mismatch");
        }
        if (!visible) {
            ++dropped;
            return false;
        }
        if (!swapchain || width != buffer_width || height != buffer_height) create_swapchain(width, height);
        const HRESULT test = swapchain->Present(0, DXGI_PRESENT_TEST);
        if (test == DXGI_STATUS_OCCLUDED) {
            ++dropped;
            return false;
        }
        check(test, "Test preview DXGI visibility");
        com_ptr<ID3D11Texture2D> backbuffer;
        check(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), backbuffer.put_void()),
            "Get preview swapchain backbuffer");
        context->UpdateSubresource(backbuffer.get(), 0, nullptr, pixels.data(), width * 4, 0);
        const HRESULT result = swapchain->Present(1, 0);
        if (result == DXGI_STATUS_OCCLUDED) {
            ++dropped;
            return false;
        }
        check(result, "Present preview swapchain");
        ++presented;
        pump_messages();
        return true;
    }
    bool present_texture(ID3D11Texture2D* texture) {
        if (!texture) throw std::invalid_argument("DXGI preview texture 为空");
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count != 1) {
            throw std::invalid_argument("DXGI preview texture 必须是单采样 BGRA8");
        }
        com_ptr<ID3D11Device> source_device;
        texture->GetDevice(source_device.put());
        if (source_device.get() != device.get()) throw std::invalid_argument("DXGI preview texture 设备不匹配");
        pump_messages();
        if (!visible) {
            ++dropped;
            return false;
        }
        if (!swapchain || description.Width != buffer_width || description.Height != buffer_height) {
            create_swapchain(description.Width, description.Height);
        }
        const HRESULT test = swapchain->Present(0, DXGI_PRESENT_TEST);
        if (test == DXGI_STATUS_OCCLUDED) {
            ++dropped;
            return false;
        }
        check(test, "Test preview DXGI visibility");
        com_ptr<ID3D11Texture2D> backbuffer;
        check(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), backbuffer.put_void()),
            "Get preview swapchain backbuffer");
        context->CopyResource(backbuffer.get(), texture);
        const HRESULT result = swapchain->Present(1, 0);
        if (result == DXGI_STATUS_OCCLUDED) {
            ++dropped;
            return false;
        }
        check(result, "Present preview swapchain");
        ++presented;
        pump_messages();
        return true;
    }
};

DxgiPreviewOverlay::DxgiPreviewOverlay(std::uintptr_t owner_window)
    : impl_(std::make_unique<Impl>(owner_window)) {}
DxgiPreviewOverlay::~DxgiPreviewOverlay() = default;
void DxgiPreviewOverlay::set_bounds(int x, int y, int width, int height, bool visible) {
    impl_->set_bounds(x, y, width, height, visible);
}
bool DxgiPreviewOverlay::present(const std::vector<std::uint8_t>& pixels,
                                 std::uint32_t width, std::uint32_t height) {
    return impl_->present(pixels, width, height);
}
ID3D11Device* DxgiPreviewOverlay::device() const { return impl_->device.get(); }
ID3D11DeviceContext* DxgiPreviewOverlay::context() const { return impl_->context.get(); }
bool DxgiPreviewOverlay::present_texture(ID3D11Texture2D* texture) {
    return impl_->present_texture(texture);
}
std::uint64_t DxgiPreviewOverlay::presented_frames() const { return impl_->presented; }
std::uint64_t DxgiPreviewOverlay::dropped_frames() const { return impl_->dropped; }

}  // namespace dogsc::cli
