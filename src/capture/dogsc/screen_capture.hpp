#pragma once

#include "dogsc/native_capture.hpp"
#include <winrt/base.h>

namespace dogsc::capture {

struct CaptureTarget {
    HMONITOR monitor = nullptr;
    HWND window = nullptr;
    // 区域坐标相对于采集项客户画面；零宽高表示完整采集项。
    RECT region{};
    bool include_cursor = false;
};

struct ScreenFrame {
    winrt::com_ptr<ID3D11Texture2D> texture;
    std::int64_t host_time = 0;
};

class ScreenCapture {
public:
    explicit ScreenCapture(const CaptureTarget& target);
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;
    ID3D11Device* device() const;
    std::uint32_t width() const;
    std::uint32_t height() const;
    void start();
    // 返回最新帧；无新帧返回空纹理。纹理在下次 poll 前可供同步消费。
    ScreenFrame poll();
    bool closed() const;
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dogsc::capture
