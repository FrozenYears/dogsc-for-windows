#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <optional>
#include <string>

#include "dogsc/output_frame_schedule.hpp"

namespace dogsc {

/// 具体摄像头采集分辨率；帧率由设备协商，录制保留样本真实时间戳。
struct CameraCaptureResolution {
    int width = 0;
    int height = 0;

    constexpr bool operator==(const CameraCaptureResolution&) const = default;

    [[nodiscard]] std::string id() const {
        return std::to_string(width) + "x" + std::to_string(height);
    }

    [[nodiscard]] std::string resolution_label() const {
        if ((width == 3840 && height == 2160) || (width == 4096 && height == 2160)) return "4K";
        if (width == 2560 && height == 1440) return "1440p";
        if (width == 1920 && height == 1080) return "1080p";
        if (width == 1280 && height == 720) return "720p";
        return std::to_string(width) + "×" + std::to_string(height);
    }

    [[nodiscard]] std::string aspect_ratio_label() const {
        const int divisor = std::gcd(std::max(std::abs(width), 1), std::max(std::abs(height), 1));
        return std::to_string(width / divisor) + ":" + std::to_string(height / divisor);
    }

    [[nodiscard]] std::string label() const {
        return resolution_label() + " · " + aspect_ratio_label() + " · " +
               std::to_string(width) + "×" + std::to_string(height);
    }
};

enum class CaptureSource {
    kDisplay,
    kWindow,
    kArea,
    kDevice,
};

[[nodiscard]] inline std::string to_raw_value(CaptureSource value) {
    switch (value) {
        case CaptureSource::kDisplay: return "整块屏幕";
        case CaptureSource::kWindow: return "单个窗口";
        case CaptureSource::kArea: return "自选区域";
        case CaptureSource::kDevice: return "iOS 设备";
    }
    return "整块屏幕";
}

[[nodiscard]] inline std::optional<CaptureSource> capture_source_from_raw(const std::string& raw) {
    if (raw == "整块屏幕") return CaptureSource::kDisplay;
    if (raw == "单个窗口") return CaptureSource::kWindow;
    if (raw == "自选区域") return CaptureSource::kArea;
    if (raw == "iOS 设备") return CaptureSource::kDevice;
    return std::nullopt;
}

enum class SystemAudioScope {
    kAll,
    kSelectedApplication,
};

[[nodiscard]] inline std::string to_raw_value(SystemAudioScope value) {
    switch (value) {
        case SystemAudioScope::kAll: return "全部系统声音";
        case SystemAudioScope::kSelectedApplication: return "所选窗口 App";
    }
    return "全部系统声音";
}

[[nodiscard]] inline std::optional<SystemAudioScope> system_audio_scope_from_raw(
    const std::string& raw) {
    if (raw == "全部系统声音") return SystemAudioScope::kAll;
    if (raw == "所选窗口 App") return SystemAudioScope::kSelectedApplication;
    return std::nullopt;
}

/// 归一化矩形，原点位于左上角。
struct NormalizedRect {
    double x = 0;
    double y = 0;
    double width = 1;
    double height = 1;

    constexpr bool operator==(const NormalizedRect&) const = default;

    [[nodiscard]] NormalizedRect constrained() const {
        const double safe_x = std::clamp(x, 0.0, 1.0);
        const double safe_y = std::clamp(y, 0.0, 1.0);
        return {safe_x, safe_y, std::min(std::max(width, 0.02), 1.0 - safe_x),
                std::min(std::max(height, 0.02), 1.0 - safe_y)};
    }
};

struct CaptureDimensions {
    int width = 0;
    int height = 0;

    constexpr bool operator==(const CaptureDimensions&) const = default;

    [[nodiscard]] static CaptureDimensions h264_compatible(
        int source_width, int source_height, int maximum_width = 4096,
        int maximum_height = 2304) {
        const int safe_width = std::max(source_width, 2);
        const int safe_height = std::max(source_height, 2);
        const double scale = std::min({1.0, static_cast<double>(maximum_width) / safe_width,
                                       static_cast<double>(maximum_height) / safe_height});
        const int width = std::max(static_cast<int>(std::floor(safe_width * scale)) / 2 * 2, 2);
        const int height = std::max(static_cast<int>(std::floor(safe_height * scale)) / 2 * 2, 2);
        return {width, height};
    }
};

enum class CaptureCodec {
    kHevc,
    kH264,
    kProRes422,
};

[[nodiscard]] inline std::string to_raw_value(CaptureCodec value) {
    switch (value) {
        case CaptureCodec::kHevc: return "HEVC";
        case CaptureCodec::kH264: return "H.264";
        case CaptureCodec::kProRes422: return "ProRes 422";
    }
    return "HEVC";
}

[[nodiscard]] inline std::optional<CaptureCodec> capture_codec_from_raw(const std::string& raw) {
    if (raw == "HEVC") return CaptureCodec::kHevc;
    if (raw == "H.264") return CaptureCodec::kH264;
    if (raw == "ProRes 422") return CaptureCodec::kProRes422;
    return std::nullopt;
}

[[nodiscard]] inline bool uses_mov_container(CaptureCodec codec) {
    return codec == CaptureCodec::kProRes422;
}

[[nodiscard]] inline std::string recording_label(CaptureCodec codec) {
    switch (codec) {
        case CaptureCodec::kHevc: return "HEVC（H.265，原生分辨率）";
        case CaptureCodec::kH264: return "H.264（兼容模式，最高 4K）";
        case CaptureCodec::kProRes422: return "ProRes 422（原生分辨率）";
    }
    return "HEVC（H.265，原生分辨率）";
}

enum class CaptureResolutionLimit {
    kNative,
    kUHD,
    kFullHD,
};

[[nodiscard]] inline std::string to_raw_value(CaptureResolutionLimit value) {
    switch (value) {
        case CaptureResolutionLimit::kNative: return "原始分辨率";
        case CaptureResolutionLimit::kUHD: return "最高 4K";
        case CaptureResolutionLimit::kFullHD: return "最高 1080p";
    }
    return "原始分辨率";
}

[[nodiscard]] inline std::optional<CaptureResolutionLimit> capture_resolution_limit_from_raw(
    const std::string& raw) {
    if (raw == "原始分辨率") return CaptureResolutionLimit::kNative;
    if (raw == "最高 4K") return CaptureResolutionLimit::kUHD;
    if (raw == "最高 1080p") return CaptureResolutionLimit::kFullHD;
    return std::nullopt;
}

[[nodiscard]] inline CaptureDimensions apply_capture_resolution_limit(
    CaptureResolutionLimit limit, CaptureDimensions size) {
    if (limit == CaptureResolutionLimit::kNative) return size;
    const int long_edge = limit == CaptureResolutionLimit::kUHD ? 3840 : 1920;
    const int short_edge = limit == CaptureResolutionLimit::kUHD ? 2160 : 1080;
    const bool landscape = size.width >= size.height;
    return CaptureDimensions::h264_compatible(
        size.width, size.height, landscape ? long_edge : short_edge,
        landscape ? short_edge : long_edge);
}

/// 录制时的来源、编码、音频与视频设备配置。
struct CaptureConfiguration {
    CaptureSource source = CaptureSource::kDisplay;
    std::optional<uint32_t> display_id;
    std::optional<std::string> display_name;
    std::optional<uint32_t> window_id;
    std::optional<std::string> window_name;
    std::optional<std::string> selected_application_bundle_identifier;
    std::optional<std::string> selected_application_name;
    std::optional<NormalizedRect> area;
    std::optional<std::string> device_id;
    std::optional<std::string> device_name;
    OutputFrameRate capture_frame_rate = OutputFrameRate::fps60;
    CaptureCodec capture_codec = CaptureCodec::kHevc;
    CaptureResolutionLimit capture_resolution_limit = CaptureResolutionLimit::kNative;
    bool hides_desktop_files = false;
    bool hides_dock = false;
    bool records_system_audio = true;
    SystemAudioScope system_audio_scope = SystemAudioScope::kAll;
    bool records_microphone = false;
    std::optional<std::string> microphone_device_id;
    std::optional<std::string> microphone_device_name;
    bool records_camera = false;
    std::optional<std::string> camera_device_id;
    std::optional<std::string> camera_device_name;
    std::optional<CameraCaptureResolution> camera_capture_resolution;

    bool operator==(const CaptureConfiguration&) const = default;
};

}  // namespace dogsc
