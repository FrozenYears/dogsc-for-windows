#include "dogsc/timeline_media_plan.hpp"

#include <algorithm>
#include <cmath>

namespace dogsc {
namespace {

constexpr TimeInterval kMicro = 0.000001;

}  // namespace

std::optional<TimeInterval> TimelineMediaSlice::source_time_at_output_time(
    TimeInterval time) const {
    if (!std::isfinite(time)) return std::nullopt;
    const bool at_or_after_start =
        time > output_start || TimelineMediaPlan::is_same_boundary(time, output_start);
    if (!at_or_after_start || time >= output_end()) return std::nullopt;
    // `slice_at_output_time` 可能把低几个 ulp 的时间点分配给本切片；
    // 这里把纯表示误差夹回起点，避免二次严格比较把已选切片变成缺口。
    const TimeInterval elapsed = std::max(time - output_start, 0.0);
    return source_start + elapsed * source_time_scale;
}

TimelineMediaPlan TimelineMediaPlan::from_primary(const TimelineMap& timeline_map) {
    TimelineMediaPlan plan;
    plan.output_duration_ = timeline_map.output_duration();
    plan.slices_.reserve(timeline_map.segments().size());
    for (const ResolvedRecordingSegment& segment : timeline_map.segments()) {
        plan.slices_.push_back(TimelineMediaSlice{
            .id = segment.id,
            .segment_id = segment.id,
            .output_start = segment.output_start,
            .source_start = segment.source_start,
            .duration = segment.output_duration(),
            .source_time_scale = segment.playback_rate,
        });
    }
    return plan;
}

TimelineMediaPlan TimelineMediaPlan::from_auxiliary(
    const TimelineMap& timeline_map,
    const MediaTimelinePlacement& placement,
    const std::vector<MediaSyncAnchor>& sync_anchors,
    const std::optional<MediaTimeRange>& source_available_range) {
    TimelineMediaPlan plan;
    plan.output_duration_ = timeline_map.output_duration();

    if (sync_anchors.empty()) {
        for (const ResolvedRecordingSegment& segment : timeline_map.segments()) {
            const TimeInterval retained_start = std::max(segment.source_start, placement.timeline_start);
            const TimeInterval retained_end = std::min(segment.source_end(), placement.timeline_end());
            const bool starts_at_primary_boundary =
                is_same_boundary(retained_start, segment.source_start);
            const bool ends_at_primary_boundary =
                is_same_boundary(retained_end, segment.source_end());
            const TimeInterval output_start =
                starts_at_primary_boundary
                    ? segment.output_start
                    : segment.output_start +
                          (retained_start - segment.source_start) / segment.playback_rate;

            TimeInterval duration = 0;
            if (starts_at_primary_boundary && ends_at_primary_boundary) {
                // 直接复用主片段时长：重算 (sourceStart + duration) - sourceStart
                // 可能短几个 ulp，在切点留下微观空洞，让 60fps 输出帧丢一帧摄像头。
                duration = segment.output_duration();
            } else {
                const TimeInterval output_end =
                    ends_at_primary_boundary
                        ? segment.output_end()
                        : segment.output_start +
                              (retained_end - segment.source_start) / segment.playback_rate;
                duration = output_end - output_start;
            }
            if (duration <= 0) continue;

            plan.slices_.push_back(TimelineMediaSlice{
                .id = segment.id,
                .segment_id = segment.id,
                .output_start = output_start,
                .source_start = placement.source_time_at(retained_start).value_or(
                    placement.source_start),
                .duration = duration,
                .source_time_scale = placement.source_time_scale * segment.playback_rate,
            });
        }
        return plan;
    }

    const MediaSyncAnchorCurve curve(sync_anchors);
    const TimeInterval available_start = source_available_range ? source_available_range->start : 0;
    const TimeInterval available_end = source_available_range
                                           ? source_available_range->end()
                                           : std::numeric_limits<TimeInterval>::max();

    for (const ResolvedRecordingSegment& segment : timeline_map.segments()) {
        const TimeInterval retained_start = std::max(segment.source_start, placement.timeline_start);
        const TimeInterval retained_end = std::min(segment.source_end(), placement.timeline_end());
        if (retained_end - retained_start <= kMicro) continue;

        std::vector<TimeInterval> boundaries;
        boundaries.push_back(retained_start);
        std::vector<TimeInterval> interior;
        for (const MediaSyncAnchor& anchor : sync_anchors) {
            if (anchor.source_time > retained_start + kMicro &&
                anchor.source_time < retained_end - kMicro) {
                interior.push_back(anchor.source_time);
            }
        }
        std::sort(interior.begin(), interior.end());
        boundaries.insert(boundaries.end(), interior.begin(), interior.end());
        boundaries.push_back(retained_end);

        for (size_t index = 0; index + 1 < boundaries.size(); ++index) {
            const TimeInterval primary_start = boundaries[index];
            const TimeInterval primary_end = boundaries[index + 1];
            const bool starts_at_primary_boundary =
                is_same_boundary(primary_start, segment.source_start);
            const bool ends_at_primary_boundary = is_same_boundary(primary_end, segment.source_end());
            const TimeInterval output_start =
                starts_at_primary_boundary
                    ? segment.output_start
                    : segment.output_start +
                          (primary_start - segment.source_start) / segment.playback_rate;
            const TimeInterval output_end =
                ends_at_primary_boundary
                    ? segment.output_end()
                    : segment.output_start +
                          (primary_end - segment.source_start) / segment.playback_rate;
            const TimeInterval duration =
                (starts_at_primary_boundary && ends_at_primary_boundary) ? segment.output_duration()
                                                                        : output_end - output_start;
            if (duration <= kMicro) continue;

            const TimeInterval base_start =
                placement.source_start +
                (primary_start - placement.timeline_start) * placement.source_time_scale;
            const TimeInterval base_end =
                placement.source_start +
                (primary_end - placement.timeline_start) * placement.source_time_scale;
            const TimeInterval corrected_start =
                std::clamp(base_start + curve.offset_at_source_time(primary_start), available_start,
                           available_end);
            const TimeInterval corrected_end =
                std::clamp(base_end + curve.offset_at_source_time(primary_end), available_start,
                           available_end);
            const TimeInterval corrected_duration = corrected_end - corrected_start;
            if (corrected_duration <= kMicro) continue;

            plan.slices_.push_back(TimelineMediaSlice{
                .id = segment.id,
                .segment_id = segment.id,
                .output_start = output_start,
                .source_start = corrected_start,
                .duration = duration,
                .source_time_scale = corrected_duration / duration,
            });
        }
    }
    return plan;
}

TimeInterval TimelineMediaPlan::playable_duration() const {
    TimeInterval total = 0;
    for (const TimelineMediaSlice& slice : slices_) total += slice.duration;
    return total;
}

std::optional<TimeInterval> TimelineMediaPlan::source_time_at_output_time(TimeInterval time) const {
    const TimelineMediaSlice* slice = slice_at_output_time(time);
    if (slice == nullptr) return std::nullopt;
    return slice->source_time_at_output_time(time);
}

bool TimelineMediaPlan::contains(TimeInterval output_time) const {
    return source_time_at_output_time(output_time).has_value();
}

const TimelineMediaSlice* TimelineMediaPlan::slice_at_output_time(TimeInterval time) const {
    if (!std::isfinite(time) || time < 0) return nullptr;
    size_t lower = 0;
    size_t upper = slices_.size();
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2;
        const TimeInterval end = slices_[middle].output_end();
        if (end < time || is_same_boundary(end, time)) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    if (lower >= slices_.size()) return nullptr;
    const TimelineMediaSlice& candidate = slices_[lower];
    const bool at_or_after_start =
        time > candidate.output_start || is_same_boundary(time, candidate.output_start);
    return at_or_after_start ? &candidate : nullptr;
}

}  // namespace dogsc
