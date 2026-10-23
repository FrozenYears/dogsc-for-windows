#include "dogsc/media_timeline.hpp"

#include <algorithm>
#include <cmath>

namespace dogsc {
namespace {

TimeInterval nonnegative(TimeInterval value) {
    return std::isfinite(value) ? std::max(value, 0.0) : 0.0;
}

}  // namespace

std::optional<TimeInterval> MediaTimelinePlacement::source_time_at(
    TimeInterval timeline_time) const {
    if (!std::isfinite(timeline_time) || timeline_time < timeline_start ||
        timeline_time >= timeline_end()) {
        return std::nullopt;
    }
    return source_start + (timeline_time - timeline_start) * source_time_scale;
}

std::optional<TimeInterval> MediaTimelinePlacement::timeline_time_for_source_time(
    TimeInterval source_time) const {
    if (!std::isfinite(source_time) || source_time < source_start ||
        source_time >= source_end() || source_time_scale <= 0) {
        return std::nullopt;
    }
    return timeline_start + (source_time - source_start) / source_time_scale;
}

bool MediaTimelinePlacement::contains(TimeInterval timeline_time) const {
    return source_time_at(timeline_time).has_value() && playable_duration > 0;
}

MediaTimelinePlacement MediaTimelinePlacement::make(
    const ProjectMediaReference& reference,
    TimeInterval source_available_start,
    TimeInterval source_available_duration,
    TimeInterval timeline_duration) {
    const TimeInterval timeline_start = nonnegative(reference.start_offset);
    const TimeInterval source_start = std::max(nonnegative(reference.source_start_time),
                                              source_available_start);
    const TimeInterval available_end = source_available_start + source_available_duration;
    const TimeInterval requested = std::max(timeline_duration - timeline_start, 0.0);
    const TimeInterval source_span = std::max(available_end - source_start, 0.0);
    MediaTimelinePlacement placement;
    placement.timeline_start = timeline_start;
    placement.source_start = source_start;

    if (reference.source_time_scale.has_value()) {
        placement.source_time_scale = *reference.source_time_scale;
        placement.playable_duration = requested;
    } else if (source_span > 0.1 && requested > 0.1) {
        // 两端锚定的推断比例；超出 0.9…1.1 视为轨不完整，保持原速。
        const double scale = source_span / requested;
        placement.source_time_scale = (scale >= 0.9 && scale <= 1.1) ? scale : 1.0;
        placement.playable_duration = requested;
    } else {
        placement.source_time_scale = 1.0;
        placement.playable_duration = std::min(requested,
                                               source_span / placement.source_time_scale);
    }
    return placement;
}

MediaSyncAnchorCurve::MediaSyncAnchorCurve(std::vector<MediaSyncAnchor> anchors)
    : anchors_(std::move(anchors)) {
    std::sort(anchors_.begin(), anchors_.end(), [](const MediaSyncAnchor& lhs,
                                                   const MediaSyncAnchor& rhs) {
        if (lhs.source_time != rhs.source_time) return lhs.source_time < rhs.source_time;
        return lhs.id.to_string() < rhs.id.to_string();
    });
}

TimeInterval MediaSyncAnchorCurve::offset_at_source_time(TimeInterval source_time) const {
    if (!std::isfinite(source_time)) return 0;
    if (anchors_.empty() || source_time <= 0) return 0;

    if (source_time < anchors_.front().source_time) {
        const MediaSyncAnchor& first = anchors_.front();
        if (first.source_time <= 0) return first.offset;
        return first.offset * (source_time / first.source_time);
    }

    for (size_t index = 0; index < anchors_.size(); ++index) {
        const MediaSyncAnchor& anchor = anchors_[index];
        if (source_time == anchor.source_time) return anchor.offset;
        if (source_time > anchor.source_time) {
            if (index + 1 >= anchors_.size()) return anchor.offset;
            const MediaSyncAnchor& next = anchors_[index + 1];
            const TimeInterval span = next.source_time - anchor.source_time;
            if (span <= 0) return next.offset;
            const double progress = (source_time - anchor.source_time) / span;
            return anchor.offset + (next.offset - anchor.offset) * progress;
        }
    }
    return anchors_.back().offset;
}

}  // namespace dogsc
