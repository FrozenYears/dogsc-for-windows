#pragma once

#include "dogsc/screen_capture.hpp"

namespace dogsc::capture {

// 调用者已初始化 COM MTA 和 Media Foundation；仅显式创建时打开首个摄像头。
// SourceReader 在 CPU 上转换 RGB32，poll 上传不透明 BGRA；不宣称 GPU 零拷贝。
class CameraCapture {
public:
    explicit CameraCapture(ID3D11Device* device);
    ~CameraCapture();
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;
    std::uint32_t width() const;
    std::uint32_t height() const;
    void start();
    // 非阻塞获取最新帧；与 WGC 一样，纹理有效且不变直到调用者下次 poll。
    ScreenFrame poll();
    // 最近一次非空 poll 是否取得设备 QPC 时间戳；首帧之前为 false。
    // false 表示回调到达时测量的主机 QPC，包含未知采集/转换延迟，不保证帧级同步。
    bool timestamp_is_device_qpc() const;
    // 终止实例，取消读取并等待异步 Flush；stop 后不可重新 start。
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dogsc::capture
