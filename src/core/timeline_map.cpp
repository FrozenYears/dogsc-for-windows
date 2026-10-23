#include "dogsc/timeline_map.hpp"

#include <algorithm>
#include <cmath>

namespace dogsc {
namespace {

/// 与上游一致：允许源区间末端越过可用时长 1/120000 秒，
/// 用来吸收时间换算误差，同时不允许真实越界。
constexpr TimeInterval kEpsilon = 1.0 / 120000.0;

Uuid make_uuid(uint8_t last_byte) {
    Uuid id;
    id.bytes[6] = 0x40;  // version 4
    id.bytes[8] = 0x80;  // variant
    id.bytes[15] = last_byte;
    return id;
}

bool source_precedes(const RecordingSegment& lhs, const RecordingSegment& rhs) {
    if (lhs.source_start != rhs.source_start) return lhs.source_start < rhs.source_start;
    return lhs.id.to_string() < rhs.id.to_string();
}

}  // namespace

const char* TimelineMapError::message() const {
    switch (code) {
        case TimelineMapErrorCode::kInvalidFullSourceDuration:
            return "主录屏时长必须是正的有限数。";
        case TimelineMapErrorCode::kEmptyEditedSequence:
            return "剪辑后至少需要保留一个主录屏片段。";
        case TimelineMapErrorCode::kDuplicateSegmentID:
            return "时间线中存在重复的主片段。";
        case TimelineMapErrorCode::kInvalidSegment:
            return "主片段包含无效的时间范围。";
        case TimelineMapErrorCode::kSegmentOutsideSource:
            return "主片段超出原始录屏可用时长。";
        case TimelineMapErrorCode::kNonMonotonicSegments:
            return "主片段不能重复使用同一段源素材。";
    }
    return "";
}

Uuid TimelineMap::full_recording_segment_id() {
    // 与上游 00000000-0000-4000-8000-000000000001 完全一致。
    Uuid id;
    id.bytes[6] = 0x40;
    id.bytes[8] = 0x80;
    id.bytes[15] = 0x01;
    return id;
}

std::optional<TimelineMapError> TimelineMap::create(const SourceSequence& source_sequence,
                                                    TimeInterval full_source_duration,
                                                    TimelineMap& out) {
    if (!std::isfinite(full_source_duration) || full_source_duration <= 0) {
        return TimelineMapError{TimelineMapErrorCode::kInvalidFullSourceDuration, {}, {}};
    }

    std::vector<RecordingSegment> authored;
    if (source_sequence.is_full_recording) {
        authored.push_back(RecordingSegment{
            .id = full_recording_segment_id(),
            .source_start = 0,
            .source_duration = full_source_duration,
            .playback_rate = 1,
        });
    } else {
        if (source_sequence.segments.empty()) {
            return TimelineMapError{TimelineMapErrorCode::kEmptyEditedSequence, {}, {}};
        }
        authored = source_sequence.segments;
    }

    std::vector<ResolvedRecordingSegment> resolved;
    resolved.reserve(authored.size());
    TimeInterval output_start = 0;

    for (size_t index = 0; index < authored.size(); ++index) {
        const RecordingSegment& segment = authored[index];
        for (size_t previous_index = 0; previous_index < index; ++previous_index) {
            if (authored[previous_index].id == segment.id) {
                return TimelineMapError{TimelineMapErrorCode::kDuplicateSegmentID, segment.id, {}};
            }
        }

        if (!std::isfinite(segment.source_start) || !std::isfinite(segment.source_duration) ||
            !std::isfinite(segment.playback_rate) || segment.source_start < 0 ||
            segment.source_duration <= 0 || segment.playback_rate <= 0 ||
            !std::isfinite(segment.source_end())) {
            return TimelineMapError{TimelineMapErrorCode::kInvalidSegment, segment.id, {}};
        }

        const TimeInterval available_duration = full_source_duration;
        if (segment.source_start >= available_duration ||
            segment.source_end() > available_duration + kEpsilon) {
            return TimelineMapError{TimelineMapErrorCode::kSegmentOutsideSource, segment.id, {}};
        }

        const TimeInterval resolved_duration =
            std::min(segment.source_duration, available_duration - segment.source_start);
        if (resolved_duration <= 0) {
            return TimelineMapError{TimelineMapErrorCode::kSegmentOutsideSource, segment.id, {}};
        }

        resolved.push_back(ResolvedRecordingSegment{
            .id = segment.id,
            .source_start = segment.source_start,
            .source_duration = resolved_duration,
            .playback_rate = segment.playback_rate,
            .output_start = output_start,
        });
        output_start += resolved_duration / segment.playback_rate;
    }

    // 每个片段都不得与「源起点更早或相同」的另一片段重叠。
    for (const RecordingSegment& current : authored) {
        const RecordingSegment* previous = nullptr;
        for (const RecordingSegment& candidate : authored) {
            if (candidate.id == current.id) continue;
            if (candidate.source_start > current.source_start) continue;
            if (previous == nullptr || source_precedes(*previous, candidate)) {
                previous = &candidate;
            }
        }
        if (previous != nullptr &&
            current.source_start < previous->source_end() - kEpsilon) {
            return TimelineMapError{TimelineMapErrorCode::kNonMonotonicSegments, current.id,
                                    previous->id};
        }
    }

    out.segments_ = std::move(resolved);
    out.full_source_duration_ = full_source_duration;
    return std::nullopt;
}

TimeInterval TimelineMap::output_duration() const {
    return segments_.empty() ? 0 : segments_.back().output_end();
}

const ResolvedRecordingSegment* TimelineMap::segment_at_output_time(TimeInterval time) const {
    if (!std::isfinite(time) || time < 0 || time >= output_duration()) return nullptr;
    size_t lower = 0;
    size_t upper = segments_.size();
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2;
        if (segments_[middle].output_end() <= time) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    if (lower >= segments_.size()) return nullptr;
    const ResolvedRecordingSegment& candidate = segments_[lower];
    return time >= candidate.output_start ? &candidate : nullptr;
}

std::optional<TimeInterval> TimelineMap::source_time_at_output_time(TimeInterval time) const {
    const ResolvedRecordingSegment* segment = segment_at_output_time(time);
    if (segment == nullptr) return std::nullopt;
    return segment->source_start + (time - segment->output_start) * segment->playback_rate;
}

std::optional<TimeInterval> TimelineMap::output_time_for_source_time(TimeInterval time) const {
    if (!std::isfinite(time)) return std::nullopt;
    for (const ResolvedRecordingSegment& segment : segments_) {
        if (time >= segment.source_start && time < segment.source_end()) {
            return segment.output_start + (time - segment.source_start) / segment.playback_rate;
        }
    }
    return std::nullopt;
}

std::optional<MediaTimeRange> TimelineMap::output_range_for_segment_id(const Uuid& id) const {
    for (const ResolvedRecordingSegment& segment : segments_) {
        if (segment.id == id) return segment.output_range();
    }
    return std::nullopt;
}

std::optional<MediaTimeRange> TimelineMap::source_range_for_segment_id(const Uuid& id) const {
    for (const ResolvedRecordingSegment& segment : segments_) {
        if (segment.id == id) return segment.source_range();
    }
    return std::nullopt;
}

SourceSequence TimelineMap::materialized_source_sequence() const {
    SourceSequence result;
    result.is_full_recording = false;
    result.segments.reserve(segments_.size());
    for (const ResolvedRecordingSegment& segment : segments_) {
        result.segments.push_back(RecordingSegment{
            .id = segment.id,
            .source_start = segment.source_start,
            .source_duration = segment.source_duration,
            .playback_rate = segment.playback_rate,
        });
    }
    return result;
}

}  // namespace dogsc
