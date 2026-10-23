#pragma once

#include <optional>
#include <string>
#include <vector>

#include "dogsc/basic_types.hpp"
#include "dogsc/core_types.hpp"
#include "dogsc/timeline_map.hpp"

namespace dogsc {

/// 一次源轨到工程时间线的规范映射。
struct MediaSyncAnchor {
    Uuid id{};
    TimeInterval source_time = 0;
    TimeInterval offset = 0;

    constexpr bool operator==(const MediaSyncAnchor&) const = default;
};

/// 工程包内的媒体引用（相对路径 + 放置参数）。
struct ProjectMediaReference {
    std::string relative_path;
    TimeInterval start_offset = 0;
    TimeInterval source_start_time = 0;
    std::optional<double> source_time_scale;
    std::optional<TimeInterval> source_end_time;
    std::vector<MediaSyncAnchor> sync_anchors;

    bool operator==(const ProjectMediaReference&) const = default;
};

/// 源轨的可用区间与放置位置。
struct MediaTimelinePlacement {
    TimeInterval timeline_start = 0;
    TimeInterval source_start = 0;
    TimeInterval playable_duration = 0;
    double source_time_scale = 1;

    constexpr bool operator==(const MediaTimelinePlacement&) const = default;

    [[nodiscard]] TimeInterval source_end() const {
        return source_start + playable_duration * source_time_scale;
    }
    [[nodiscard]] TimeInterval timeline_end() const { return timeline_start + playable_duration; }

    [[nodiscard]] std::optional<TimeInterval> source_time_at(TimeInterval timeline_time) const;
    [[nodiscard]] std::optional<TimeInterval> timeline_time_for_source_time(
        TimeInterval source_time) const;
    [[nodiscard]] bool contains(TimeInterval timeline_time) const;

    static MediaTimelinePlacement make(const ProjectMediaReference& reference,
                                       TimeInterval source_available_start,
                                       TimeInterval source_available_duration,
                                       TimeInterval timeline_duration);
};

/// 按原始录屏源时钟求值分段线性相机时钟校正。
/// 首个锚点之前，校正自源时间 0 处的 0 开始增长；末个锚点之后保持最后一次校正。
class MediaSyncAnchorCurve {
public:
    explicit MediaSyncAnchorCurve(std::vector<MediaSyncAnchor> anchors);

    [[nodiscard]] TimeInterval offset_at_source_time(TimeInterval source_time) const;

    constexpr bool operator==(const MediaSyncAnchorCurve&) const = default;

private:
    std::vector<MediaSyncAnchor> anchors_;
};

}  // namespace dogsc
