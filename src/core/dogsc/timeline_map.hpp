#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dogsc/basic_types.hpp"
#include "dogsc/core_types.hpp"

namespace dogsc {

/// 128-bit UUID，与 Swift `UUID` 的字符串表示一致。
struct Uuid {
    std::array<uint8_t, 16> bytes{};

    constexpr bool operator==(const Uuid&) const = default;

    [[nodiscard]] static std::optional<Uuid> parse(const std::string& text);
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] bool is_nil() const;

    static constexpr Uuid nil() { return {}; }
};

enum class TimelineMapErrorCode {
    kInvalidFullSourceDuration,
    kEmptyEditedSequence,
    kDuplicateSegmentID,
    kInvalidSegment,
    kSegmentOutsideSource,
    kNonMonotonicSegments,
};

/// 带有具体当事片段 ID 的错误，便于上层给出可定位的提示。
struct TimelineMapError {
    TimelineMapErrorCode code = TimelineMapErrorCode::kInvalidFullSourceDuration;
    Uuid subject{};
    Uuid previous{};

    constexpr bool operator==(const TimelineMapError&) const = default;

    [[nodiscard]] const char* message() const;
};

/// 原始录屏中被保留的一段；顺序即输出顺序。
///
/// 刻意不持久化 `outputStart`：它由时长累加推导，
/// 这样裁切或波纹删除不会留下两个互相矛盾的起点。
struct RecordingSegment {
    Uuid id{};
    TimeInterval source_start = 0;
    TimeInterval source_duration = 0;
    /// 输出一秒消耗多少源秒；编辑器允许 1×…100× 快进。
    double playback_rate = 1;

    constexpr bool operator==(const RecordingSegment&) const = default;

    [[nodiscard]] TimeInterval source_end() const { return source_start + source_duration; }
    [[nodiscard]] TimeInterval output_duration() const { return source_duration / playback_rate; }
};

/// 主录屏的持久化剪辑决定。
struct SourceSequence {
    bool is_full_recording = true;
    std::vector<RecordingSegment> segments;

    constexpr bool operator==(const SourceSequence&) const = default;

    static SourceSequence full_recording() { return {}; }
};

/// 已解析的主片段：`outputStart` 由累加得到。
struct ResolvedRecordingSegment {
    Uuid id{};
    TimeInterval source_start = 0;
    TimeInterval source_duration = 0;
    double playback_rate = 1;
    TimeInterval output_start = 0;

    constexpr bool operator==(const ResolvedRecordingSegment&) const = default;

    [[nodiscard]] TimeInterval source_end() const { return source_start + source_duration; }
    [[nodiscard]] TimeInterval output_duration() const { return source_duration / playback_rate; }
    [[nodiscard]] TimeInterval output_end() const { return output_start + output_duration(); }
    [[nodiscard]] MediaTimeRange source_range() const { return {source_start, source_duration}; }
    [[nodiscard]] MediaTimeRange output_range() const { return {output_start, output_duration()}; }
};

/// 最终波纹时间线与原始录制时钟之间的纯映射。
///
/// 数组顺序即输出顺序；源区间可重排但不重叠，
/// 保证原始素材的每个瞬间只有一个归属。
class TimelineMap {
public:
    /// 未剪辑工程使用的固定片段 ID。
    static Uuid full_recording_segment_id();

    /// 构造失败时返回错误，成功时通过输出参数返回实例。
    static std::optional<TimelineMapError> create(const SourceSequence& source_sequence,
                                                  TimeInterval full_source_duration,
                                                  TimelineMap& out);

    [[nodiscard]] const std::vector<ResolvedRecordingSegment>& segments() const { return segments_; }
    [[nodiscard]] TimeInterval full_source_duration() const { return full_source_duration_; }
    [[nodiscard]] TimeInterval output_duration() const;

    [[nodiscard]] const ResolvedRecordingSegment* segment_at_output_time(
        TimeInterval time) const;
    [[nodiscard]] std::optional<TimeInterval> source_time_at_output_time(
        TimeInterval time) const;
    [[nodiscard]] std::optional<TimeInterval> output_time_for_source_time(
        TimeInterval time) const;
    [[nodiscard]] std::optional<MediaTimeRange> output_range_for_segment_id(const Uuid& id) const;
    [[nodiscard]] std::optional<MediaTimeRange> source_range_for_segment_id(const Uuid& id) const;
    [[nodiscard]] SourceSequence materialized_source_sequence() const;

    constexpr bool operator==(const TimelineMap&) const = default;

private:
    std::vector<ResolvedRecordingSegment> segments_;
    TimeInterval full_source_duration_ = 0;
};

}  // namespace dogsc
