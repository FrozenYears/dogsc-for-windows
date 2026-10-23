#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "dogsc/core_types.hpp"

namespace dogsc {

enum class OutputFrameRate : int32_t {
    fps30 = 30,
    fps60 = 60,
    fps90 = 90,
    fps120 = 120,
};

enum class OutputFrameScheduleError {
    kInvalidDuration,
    kDurationTooLong,
};

/// 导出视频帧数与 PTS 的唯一真相。
///
/// 区间是端开区间：帧 `n` 存在当且仅当 `0 <= n / frameRate < duration`。
/// PTS 由整数 tick 网格推导，而不是浮点帧时长累加。
class OutputFrameSchedule {
public:
    struct Frame {
        int index = 0;
        int64_t presentation_time_value = 0;
        int32_t presentation_timescale = kPresentationTimescale;

        [[nodiscard]] double presentation_time_seconds() const {
            return static_cast<double>(presentation_time_value) /
                   static_cast<double>(presentation_timescale);
        }

        constexpr bool operator==(const Frame&) const = default;
    };

    /// 构造失败时返回错误码，成功时通过输出参数返回实例。
    static std::optional<OutputFrameScheduleError> create(double duration,
                                                         OutputFrameRate frame_rate,
                                                         OutputFrameSchedule& out);

    [[nodiscard]] std::optional<Frame> frame_at(int index) const;

    [[nodiscard]] double duration() const { return duration_; }
    [[nodiscard]] OutputFrameRate frame_rate() const { return frame_rate_; }
    [[nodiscard]] int frame_count() const { return frame_count_; }
    [[nodiscard]] int64_t ticks_per_frame() const { return ticks_per_frame_; }

    constexpr bool operator==(const OutputFrameSchedule&) const = default;

    OutputFrameSchedule() = default;

private:
    OutputFrameSchedule(double duration, OutputFrameRate frame_rate, int frame_count,
                        int64_t ticks_per_frame)
        : duration_(duration),
          frame_rate_(frame_rate),
          frame_count_(frame_count),
          ticks_per_frame_(ticks_per_frame) {}

    double duration_ = 0;
    OutputFrameRate frame_rate_ = OutputFrameRate::fps60;
    int frame_count_ = 0;
    int64_t ticks_per_frame_ = 0;
};

}  // namespace dogsc
