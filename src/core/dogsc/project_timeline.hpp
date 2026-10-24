#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>


#include "dogsc/basic_types.hpp"
#include "dogsc/core_types.hpp"
#include "dogsc/timeline_map.hpp"

namespace dogsc {

/// 缓动预设的持久化原始值即中文名，必须逐字节保持。
enum class ZoomEasingPreset {
    kLinear,
    kQuadratic,
    kCubic,
    kQuintic,
    kSpring,
    kCustom,
};

enum class ZoomKeyframeOrigin {
    kAutomatic,
    kManual,
};

enum class ElementMotionCurve {
    kSmooth,
    kSwift,
    kGentle,
};

enum class CameraShape {
    kSquare,
    kHorizontal,
    kVertical,
    kOriginal,
    kCircle,
};

/// 三次贝塞尔缓动曲线，控制点固定在 [0,1]²。
struct ZoomBezierCurve {
    double x1 = 0.65;
    double y1 = 0;
    double x2 = 0.35;
    double y2 = 1;

    constexpr bool operator==(const ZoomBezierCurve&) const = default;

    static ZoomBezierCurve linear();
    static ZoomBezierCurve quadratic();
    static ZoomBezierCurve cubic();
    static ZoomBezierCurve quintic();

    [[nodiscard]] NormalizedPoint point_at(double parameter) const;
    /// Newton-Raphson 优先，二分法兜底，保证扁平手柄也稳定收敛。
    [[nodiscard]] double value_at(double normalized_time) const;

    static ZoomBezierCurve for_preset(ZoomEasingPreset preset);
};

/// 场景转场共享的 enter / hold / return 时间参数。
struct TransitionTiming {
    TimeInterval start_time = 0;
    /// 整段时长（进入过渡 + 保持）；拖动时间线两端改变的就是它。
    TimeInterval duration = 0.7;
    /// 进入过渡时长；0 表示直接跳变；超过 duration 时按 duration 截断。
    TimeInterval lead_in_duration = 0.7;
    double lead_in_progress_offset = 0;
    ZoomEasingPreset easing = ZoomEasingPreset::kCubic;
    ZoomBezierCurve custom_curve = ZoomBezierCurve::cubic();
    TimeInterval return_duration = 0;
    double return_progress_offset = 0;
    std::optional<TimeInterval> preferred_lead_in_duration;
    std::optional<TimeInterval> preferred_return_duration;

    constexpr bool operator==(const TransitionTiming&) const = default;

    [[nodiscard]] TimeInterval end_time() const { return start_time + duration; }
    [[nodiscard]] TimeInterval lead_in_end_time() const;
    [[nodiscard]] TimeInterval effect_end_time() const { return end_time() + return_duration; }
    [[nodiscard]] bool contains(TimeInterval time) const {
        return time >= start_time && time < end_time();
    }
    [[nodiscard]] TimeInterval requested_lead_in_duration() const {
        return preferred_lead_in_duration.value_or(lead_in_duration);
    }
    [[nodiscard]] TimeInterval requested_return_duration(
        TimeInterval default_transition = 0.7) const;
    void preserve_transition_intent(TimeInterval default_transition = 0.7);
};

/// 元素运动曲线求值，入场与退场共用同一条非线性路径。
[[nodiscard]] double element_motion_progress(double linear_progress, ElementMotionCurve curve);

/// 单段主片段上的可选音源覆盖；缺省值继承工程级音量设置。
struct PrimarySegmentAudioOverrides {
    std::optional<double> system_volume;
    std::optional<double> microphone_volume;
    std::optional<bool> is_system_muted;
    std::optional<bool> is_microphone_muted;

    constexpr bool operator==(const PrimarySegmentAudioOverrides&) const = default;

    [[nodiscard]] bool is_empty() const {
        return !system_volume && !microphone_volume && !is_system_muted &&
               !is_microphone_muted;
    }
};


enum class FocusEffectTarget {
    kAnimation,
    kPointer,
    kFixed,
};

enum class FocusEffectShape {
    kRadial,
    kLinear,
};

/// 可选的局部焦点效果；合法性规则与上游 FocusEffect.isValid 一致。
struct FocusEffect {
    std::optional<FocusEffectShape> shape;
    std::optional<double> angle_degrees;
    FocusEffectTarget target = FocusEffectTarget::kAnimation;
    NormalizedPoint center{0.5, 0.5};
    double size = 0.45;
    double blur = 0.35;
    double softness = 0.5;
    double dimming = 0.18;

    bool operator==(const FocusEffect&) const = default;

    [[nodiscard]] bool is_valid() const {
        const auto unit_interval = [](double value) {
            return std::isfinite(value) && value >= 0 && value <= 1;
        };
        return unit_interval(center.x) && unit_interval(center.y) &&
               unit_interval(blur) && unit_interval(softness) && unit_interval(dimming) &&
               (!angle_degrees ||
                (std::isfinite(*angle_degrees) && *angle_degrees >= 0 && *angle_degrees <= 360)) &&
               std::isfinite(size) && size >= 0.1 && size <= 1;
    }
};

struct ScreenMotionState {
    NormalizedPoint position{0.5, 0.5};
    double scale = 1;
    double rotation_x = 0;
    double rotation_y = 0;
    double rotation_z = 0;
    double perspective = 0.65;

    constexpr bool operator==(const ScreenMotionState&) const = default;
};

struct ScreenMotionClip {
    Uuid id{};
    TransitionTiming timing{};
    ScreenMotionState target{};
    std::optional<Uuid> group_id;
    std::optional<FocusEffect> focus_effect;

    bool operator==(const ScreenMotionClip&) const = default;
};

struct CameraLayoutMode {
    enum class Kind {
        kShape,
        kFullscreen,
    };

    Kind kind = Kind::kShape;
    CameraShape shape = CameraShape::kSquare;

    constexpr bool operator==(const CameraLayoutMode&) const = default;

    static constexpr CameraLayoutMode shaped(CameraShape value) {
        return {Kind::kShape, value};
    }
    static constexpr CameraLayoutMode fullscreen() {
        return {Kind::kFullscreen, CameraShape::kSquare};
    }
};

struct CameraMotionState {
    CameraLayoutMode layout{};
    NormalizedPoint position{0.86, 0.82};
    double size = 0.35;
    double roundness = 0.5;
    double opacity = 1;

    constexpr bool operator==(const CameraMotionState&) const = default;
};

struct CameraMotionClip {
    Uuid id{};
    TransitionTiming timing{};
    CameraMotionState target{};
    std::optional<Uuid> group_id;

    bool operator==(const CameraMotionClip&) const = default;
};

struct OverlayTiming {
    TimeInterval start_time = 0;
    TimeInterval duration = 3;

    constexpr bool operator==(const OverlayTiming&) const = default;

    [[nodiscard]] TimeInterval end_time() const { return start_time + duration; }
    [[nodiscard]] bool contains(TimeInterval time) const {
        return time >= start_time && time < end_time();
    }
};

enum class MosaicEffectStyle {
    kBlur,
    kSpotlight,
};

enum class MosaicTransitionStyle {
    kNone,
    kLinear,
    kSmooth,
};

struct MosaicClip {
    Uuid id{};
    OverlayTiming timing{};
    NormalizedOverlayRect source_rect = NormalizedOverlayRect::centered();
    double corner_radius = 0.08;
    MosaicEffectStyle style = MosaicEffectStyle::kBlur;
    double intensity = 0.55;
    double spotlight_dimming = 0.22;
    MosaicTransitionStyle transition_style = MosaicTransitionStyle::kNone;
    TimeInterval transition_in_duration = 0.30;
    TimeInterval transition_out_duration = 0.30;

    constexpr bool operator==(const MosaicClip&) const = default;
};

enum class StickerAnimationPreset {
    kNone,
    kFade,
    kPop,
    kSlideLeft,
    kSlideRight,
    kSlideUp,
    kSlideDown,
    kSlideTopLeft,
    kSlideTopRight,
    kSlideBottomLeft,
    kSlideBottomRight,
};

struct StickerClip {
    Uuid id{};
    OverlayTiming timing{};
    std::string relative_path;
    NormalizedPoint position{0.5, 0.5};
    double width = 0.38;
    double rotation_degrees = 0;
    double opacity = 1;
    double corner_radius = 18;
    double border_width = 0;
    HexColor border_color = HexColor::white();
    double shadow_opacity = 0.28;
    double shadow_radius = 24;
    double shadow_offset_x = 0;
    double shadow_offset_y = 10;
    StickerAnimationPreset animation = StickerAnimationPreset::kPop;
    ElementMotionCurve animation_curve = ElementMotionCurve::kSwift;
    std::optional<StickerAnimationPreset> exit_animation;
    TimeInterval enter_duration = 0.7;
    TimeInterval exit_duration = 0.22;
    double backdrop_blur = 20;
    bool backdrop_blur_includes_camera = false;
    bool hides_screen = false;
    bool hides_camera = false;
    int layer_index = 0;

    bool operator==(const StickerClip&) const = default;

    [[nodiscard]] StickerAnimationPreset automatic_exit() const {
        switch (animation) {
            case StickerAnimationPreset::kSlideLeft:
                return StickerAnimationPreset::kSlideRight;
            case StickerAnimationPreset::kSlideRight:
                return StickerAnimationPreset::kSlideLeft;
            case StickerAnimationPreset::kSlideUp:
                return StickerAnimationPreset::kSlideDown;
            case StickerAnimationPreset::kSlideDown:
                return StickerAnimationPreset::kSlideUp;
            case StickerAnimationPreset::kSlideTopLeft:
                return StickerAnimationPreset::kSlideBottomRight;
            case StickerAnimationPreset::kSlideTopRight:
                return StickerAnimationPreset::kSlideBottomLeft;
            case StickerAnimationPreset::kSlideBottomLeft:
                return StickerAnimationPreset::kSlideTopRight;
            case StickerAnimationPreset::kSlideBottomRight:
                return StickerAnimationPreset::kSlideTopLeft;
            case StickerAnimationPreset::kNone:
            case StickerAnimationPreset::kFade:
            case StickerAnimationPreset::kPop:
                return animation;
        }
        return StickerAnimationPreset::kPop;
    }
};

struct UuidLess {
    bool operator()(const Uuid& lhs, const Uuid& rhs) const {
        return lhs.bytes < rhs.bytes;
    }
};
}  // namespace dogsc
