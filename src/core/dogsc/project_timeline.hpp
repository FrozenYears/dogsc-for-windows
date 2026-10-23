#pragma once

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

}  // namespace dogsc
