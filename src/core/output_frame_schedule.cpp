#include "dogsc/output_frame_schedule.hpp"

#include <cmath>
#include <limits>

namespace dogsc {
namespace {

/// 与上游一致的边界吸附容差：只用 ulp 量级吸收表示噪声，
/// 不使用固定 epsilon，以免抹掉真实的亚帧尾巴。
constexpr double kBoundaryUlps = 4.0;

}  // namespace

std::optional<OutputFrameScheduleError> OutputFrameSchedule::create(
    double duration, OutputFrameRate frame_rate, OutputFrameSchedule& out) {
    if (!std::isfinite(duration) || duration < 0) {
        return OutputFrameScheduleError::kInvalidDuration;
    }

    const int64_t timescale = static_cast<int64_t>(kPresentationTimescale);
    const int64_t rate = static_cast<int64_t>(frame_rate);
    if (rate <= 0 || timescale % rate != 0) {
        // 上游用 precondition 保证；这里按无效时长处理，避免 UB。
        return OutputFrameScheduleError::kInvalidDuration;
    }

    const int64_t ticks_per_frame = timescale / rate;
    const double scaled_duration = duration * static_cast<double>(rate);
    const int64_t maximum_frame_count = std::numeric_limits<int64_t>::max() /
                                            ticks_per_frame +
                                        1;
    if (!std::isfinite(scaled_duration) ||
        scaled_duration > static_cast<double>(maximum_frame_count)) {
        return OutputFrameScheduleError::kDurationTooLong;
    }

    // 形如 (1/90)*90 的值在概念上是整数，但可能相差几个二进制 ulp。
    // 只吸附这一级别的表示噪声。
    const double nearest_integer = std::round(scaled_duration);
    const double boundary_tolerance =
        std::fmax(std::nextafter(scaled_duration, scaled_duration * 2) - scaled_duration,
                  std::nextafter(nearest_integer, nearest_integer * 2) - nearest_integer) *
        kBoundaryUlps;
    const double boundary_safe_duration =
        std::fabs(scaled_duration - nearest_integer) <= boundary_tolerance ? nearest_integer
                                                                         : scaled_duration;
    const int frame_count =
        boundary_safe_duration > 0 ? static_cast<int>(std::ceil(boundary_safe_duration)) : 0;

    if (frame_count > 0) {
        const int64_t last_frame_index = static_cast<int64_t>(frame_count - 1);
        if (last_frame_index > std::numeric_limits<int64_t>::max() / ticks_per_frame) {
            return OutputFrameScheduleError::kDurationTooLong;
        }
    }

    out = OutputFrameSchedule(duration, frame_rate, frame_count, ticks_per_frame);
    return std::nullopt;
}

std::optional<OutputFrameSchedule::Frame> OutputFrameSchedule::frame_at(int index) const {
    if (index < 0 || index >= frame_count_) {
        return std::nullopt;
    }
    return Frame{
        .index = index,
        .presentation_time_value = static_cast<int64_t>(index) * ticks_per_frame_,
        .presentation_timescale = kPresentationTimescale,
    };
}

}  // namespace dogsc
