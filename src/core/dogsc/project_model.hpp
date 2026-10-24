#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dogsc/capture_configuration.hpp"
#include "dogsc/core_types.hpp"
#include "dogsc/cursor_asset.hpp"
#include "dogsc/frame_scene_evaluator.hpp"
#include "dogsc/project_timeline.hpp"
#include "dogsc/media_timeline.hpp"
#include "dogsc/timeline_map.hpp"

namespace dogsc {

/// schema 23 是第一个刻意精简的工程契约。
/// 版本 24 增加自定义画布比例；27 持久化录制标记。
struct ProjectSchema {
    static constexpr int kMinimumSupportedVersion = 23;
    static constexpr int kCurrentVersion = 27;
};

enum class ProjectSchemaErrorCode {
    kUnsupportedLegacyVersion,
    kUnsupportedFutureVersion,
    kRefusingToEncodeUnsupportedVersion,
    kRefusingToEncodeFutureVersion,
};

struct ProjectSchemaError {
    ProjectSchemaErrorCode code = ProjectSchemaErrorCode::kUnsupportedLegacyVersion;
    int found = 0;
    int bound = 0;

    constexpr bool operator==(const ProjectSchemaError&) const = default;

    [[nodiscard]] std::string message() const;

    static std::optional<ProjectSchemaError> validate_for_decoding(int version);
    static std::optional<ProjectSchemaError> validate_for_encoding(int version);
};

/// 工程持久化枚举；落盘字符串即界面显示名，必须逐字节保持。
enum class CanvasAspectRatio {
    kAdaptive,
    kLandscape,
    kStandard,
    kPortrait,
    kSquare,
    kStandardPortrait,
    kCinema,
    kCinemaPortrait,
    kCustom,
};

enum class CanvasResolution {
    kSource,
    kFullHD,
    kQuadHD,
    kUltraHD,
};

[[nodiscard]] std::string to_raw_value(CanvasAspectRatio value);
[[nodiscard]] std::optional<CanvasAspectRatio> canvas_aspect_ratio_from_raw(
    const std::string& raw);

[[nodiscard]] std::string to_raw_value(CanvasResolution value);
[[nodiscard]] std::optional<CanvasResolution> canvas_resolution_from_raw(const std::string& raw);

/// 画布尺寸（像素），长宽均为偶数。
struct CanvasDimensions {
    int width = 1920;
    int height = 1080;

    constexpr bool operator==(const CanvasDimensions&) const = default;
};

/// 自定义宽高比，缺省回落到 16:9。
struct CanvasCustomAspectRatio {
    double width = 16;
    double height = 9;

    constexpr bool operator==(const CanvasCustomAspectRatio&) const = default;
};

/// 背景来源。工程相对路径与系统绝对路径刻意分开：
/// 用户文件随工程迁移，系统媒体永不复制进工程包。
enum class BackgroundSourceKind {
    kPattern,
    kDynamicFlow,
    kProjectImage,
    kSystemImage,
    kProjectVideo,
    kSystemVideo,
};

struct BackgroundSource {
    BackgroundSourceKind kind = BackgroundSourceKind::kPattern;
    std::string pattern_preset;
    std::string dynamic_preset;
    std::string relative_path;
    std::string absolute_path;

    bool operator==(const BackgroundSource&) const = default;

    [[nodiscard]] bool is_image() const {
        return kind == BackgroundSourceKind::kProjectImage ||
               kind == BackgroundSourceKind::kSystemImage;
    }
    [[nodiscard]] bool is_video() const {
        return kind == BackgroundSourceKind::kProjectVideo ||
               kind == BackgroundSourceKind::kSystemVideo;
    }

    static BackgroundSource safe_fallback();
};

struct ExportSettings {
    int frame_rate = 60;
    CanvasResolution resolution = CanvasResolution::kSource;

    constexpr bool operator==(const ExportSettings&) const = default;
};

struct RecordingMarker {
    Uuid id{};
    TimeInterval source_time = 0;
    int number = 1;

    constexpr bool operator==(const RecordingMarker&) const = default;
};

struct AudioStyle {
    double system_volume = 0.82;
    double microphone_volume = 0.9;
    bool is_system_muted = false;
    bool is_microphone_muted = false;

    constexpr bool operator==(const AudioStyle&) const = default;
};


struct CameraStyle {
    CameraShape shape = CameraShape::kSquare;
    NormalizedPoint position{0.96, 0.47};
    double size = 0.29;
    double border_width = 3;
    double shadow_strength = 0.25;
    double roundness = 0.5;
    bool is_mirrored = true;
    bool is_hidden = false;
    double scale_during_zoom = 0.7;
    NormalizedPoint content_position{0.5, 0.5};
    double content_scale = 1;

    bool operator==(const CameraStyle&) const = default;
};

enum class CursorClickEffectStyle {
    kRipple,
    kGlow,
    kPulse,
    kBurst,
    kNone,
};

struct CursorStyle {
    CursorAssetID asset_id = CursorAssetID::automatic();
    double size = 1.25;
    bool hide_when_idle = false;
    double idle_delay = 1.2;
    CursorClickEffectStyle click_effect_style = CursorClickEffectStyle::kRipple;
    std::optional<HexColor> click_color;
    double click_opacity = 0.8;
    double click_scale = 1.0;
    double motion_tilt_strength = 2.0;

    bool operator==(const CursorStyle&) const = default;
};

/// 画布样式：舞台比例、背景、内边距、屏幕卡片外观。
struct CanvasStyle {
    CanvasAspectRatio aspect_ratio = CanvasAspectRatio::kAdaptive;
    CanvasCustomAspectRatio custom_aspect_ratio{};
    BackgroundSource background_source = BackgroundSource::safe_fallback();
    double padding = 100;
    double content_scale = 1;
    NormalizedPoint content_position{0.5, 0.5};
    NormalizedCrop crop = NormalizedCrop::full();
    double corner_radius = 24;
    double border_width = 0;
    HexColor border_color = HexColor::white();
    double shadow_strength = 0.28;
    double background_blur = 0;
    double inset_opacity = 0.125;
    std::string screen_frame = "none";
    double screen_frame_scale = 1;
    double screen_frame_toolbar_scale = 1;
    std::optional<double> screen_frame_outer_corner_radius;
    std::optional<double> screen_frame_content_corner_radius;
    std::string screen_frame_title;
    std::string screen_frame_browser_address;
    double pattern_scale = 1;
    double pattern_opacity = 1;

    bool operator==(const CanvasStyle&) const = default;

    /// 按导出分辨率解析最终像素尺寸。
    /// `source` 用真实录制像素；固定比例在裁剪包络内等比适配，从不放大。
    [[nodiscard]] CanvasDimensions pixel_dimensions(
        CanvasResolution resolution, std::optional<double> source_aspect_ratio = std::nullopt,
        std::optional<CanvasDimensions> source_pixel_size = std::nullopt) const;
};

enum class ScreenMotionStyle {
    kFocused,
    kSmooth,
};

enum class CursorMotionStyle {
    kSmooth,
    kMedium,
    kRapid,
    kNone,
};

struct MotionStyle {
    ScreenMotionStyle screen = ScreenMotionStyle::kFocused;
    CursorMotionStyle cursor = CursorMotionStyle::kSmooth;
    double screen_spring_mass = 2.4;
    double screen_spring_stiffness = 210;
    double screen_spring_damping = 42;
    double cursor_spring_mass = 2.8;
    double cursor_spring_stiffness = 450;
    double cursor_spring_damping = 66;
    ZoomEasingPreset default_zoom_easing = ZoomEasingPreset::kSpring;
    TimeInterval default_zoom_transition_duration = 0.7;
    std::optional<double> default_zoom_scale;

    bool operator==(const MotionStyle&) const = default;
};

/// 一段缩放动画，持久化于 `timeline.zoomClips`。
struct ZoomClip {
    Uuid id{};
    TimeInterval start_time = 0;
    TimeInterval end_time = 0;
    double scale = 1.6;
    NormalizedPoint focus{0.5, 0.5};
    ZoomKeyframeOrigin origin = ZoomKeyframeOrigin::kManual;
    ZoomEasingPreset easing = ZoomEasingPreset::kCubic;
    ZoomBezierCurve custom_curve = ZoomBezierCurve::cubic();
    TimeInterval enter_duration = 0.7;
    TimeInterval exit_duration = 0.7;
    double enter_progress_offset = 0;
    double exit_progress_offset = 0;
    std::optional<TimeInterval> preferred_enter_duration;
    std::optional<TimeInterval> preferred_exit_duration;
    std::optional<FocusEffect> focus_effect;

    bool operator==(const ZoomClip&) const = default;

    [[nodiscard]] TimeInterval duration() const { return std::max(end_time - start_time, 0.0); }
    [[nodiscard]] TimeInterval effect_end_time() const { return end_time + exit_duration; }
};

// 屏幕引用是完整工程的必需项；未录制工程不创建部分清单。
struct ProjectMediaManifest {
    ProjectMediaReference screen;
    std::optional<ProjectMediaReference> camera;
    std::optional<ProjectMediaReference> microphone;
    std::optional<ProjectMediaReference> pointer_events;

    bool operator==(const ProjectMediaManifest&) const = default;
};

/// 工程的完整持久化形态。
struct RecorderProject {
    int version = ProjectSchema::kCurrentVersion;
    std::string title = "未命名录制";
    /// ISO-8601 UTC 字符串；M0 原样保留，不做时区换算。
    std::string created_at;
    CaptureConfiguration capture{};
    CanvasStyle canvas{};
    CameraStyle camera{};
    AudioStyle audio{};
    MotionStyle motion{};
    OpeningSequence opening_sequence{};
    CursorStyle cursor_style{};
    ExportSettings export_settings{};
    SourceSequence source_sequence{};
    std::map<Uuid, PrimarySegmentAudioOverrides, UuidLess> primary_segment_audio_overrides;
    std::vector<ScreenMotionClip> screen_motion_clips;
    std::vector<CameraMotionClip> camera_motion_clips;
    std::vector<MosaicClip> mosaic_clips;
    std::vector<StickerClip> sticker_clips;
    std::vector<ZoomClip> zoom_clips;
    std::vector<RecordingMarker> recording_markers{};
    std::optional<ProjectMediaManifest> media;

    bool operator==(const RecorderProject&) const = default;
};

}  // namespace dogsc
