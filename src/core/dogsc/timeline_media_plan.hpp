#pragma once

#include <optional>
#include <vector>

#include "dogsc/basic_types.hpp"
#include "dogsc/core_types.hpp"
#include "dogsc/media_timeline.hpp"
#include "dogsc/timeline_map.hpp"

namespace dogsc {

/// 波纹输出时间线上一段被保留的源区间。
/// 同一份计划同时驱动预览合成与确定性导出读取。
struct TimelineMediaSlice {
    Uuid id{};
    Uuid segment_id{};
    TimeInterval output_start = 0;
    TimeInterval source_start = 0;
    TimeInterval duration = 0;
    double source_time_scale = 1;

    constexpr bool operator==(const TimelineMediaSlice&) const = default;

    [[nodiscard]] TimeInterval output_end() const { return output_start + duration; }
    [[nodiscard]] TimeInterval source_end() const { return source_start + duration * source_time_scale; }
    [[nodiscard]] std::optional<TimeInterval> source_time_at_output_time(
        TimeInterval time) const;
};

/// 由唯一权威 `TimelineMap` 生成的最终时间媒体放置。
/// 任何预览/导出实现都不得在此之后另造偏移或剪切映射。
class TimelineMediaPlan {
public:
    /// 只用于把算术结果吸附回已有的主片段切点；
    /// 它远小于任何媒体帧，因此真实的素材缺口仍保持为缺口。
    static constexpr TimeInterval kNumericalBoundaryTolerance = 1e-9;

    /// 由主时间线生成。
    static TimelineMediaPlan from_primary(const TimelineMap& timeline_map);

    /// 把辅助摄像头/麦克风放置与每个保留主片段求交。
    /// 缺口仍是缺口，存活的切片与录屏共享同一输出时钟。
    static TimelineMediaPlan from_auxiliary(const TimelineMap& timeline_map,
                                            const MediaTimelinePlacement& placement,
                                            const std::vector<MediaSyncAnchor>& sync_anchors = {},
                                            const std::optional<MediaTimeRange>& source_available_range =
                                                std::nullopt);

    [[nodiscard]] TimeInterval output_duration() const { return output_duration_; }
    [[nodiscard]] const std::vector<TimelineMediaSlice>& slices() const { return slices_; }
    [[nodiscard]] TimeInterval playable_duration() const;
    [[nodiscard]] std::optional<TimeInterval> source_time_at_output_time(TimeInterval time) const;
    [[nodiscard]] bool contains(TimeInterval output_time) const;
    [[nodiscard]] const TimelineMediaSlice* slice_at_output_time(TimeInterval time) const;

    /// 判断两个时间点是否落在同一已作者切点（容差内）。
    static bool is_same_boundary(TimeInterval lhs, TimeInterval rhs) {
        return std::abs(lhs - rhs) <= kNumericalBoundaryTolerance;
    }

    constexpr bool operator==(const TimelineMediaPlan&) const = default;

private:
    TimeInterval output_duration_ = 0;
    std::vector<TimelineMediaSlice> slices_;
};

}  // namespace dogsc
