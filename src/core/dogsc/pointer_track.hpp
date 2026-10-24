#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "dogsc/timeline_map.hpp"

#include "dogsc/basic_types.hpp"
#include "dogsc/cursor_asset.hpp"

namespace dogsc {
struct MotionStyle;
struct CursorStyle;
struct ZoomClip;

enum class PointerEventKind { kMove, kLeftClick, kRightClick };
enum class PointerModifier { kCommand, kOption, kControl, kShift, kCapsLock, kFunction };

/// 调用方须先将原始采集时钟重映射到输出时间线；同一时刻保留输入稳定顺序。
struct PointerEventRecord {
    TimeInterval time = 0;
    NormalizedPoint location{};
    PointerEventKind kind = PointerEventKind::kMove;
    std::vector<PointerModifier> modifiers;
    std::optional<CursorAssetID> cursor_asset_id;
    bool operator==(const PointerEventRecord&) const = default;
};

/// 保留时间线的每段输出事件；source/output 两域均显式保留。
struct PointerTrackSegmentRecord {
    Uuid id{};
    MediaTimeRange output_range{};
    std::vector<PointerEventRecord> source_events;
    std::vector<PointerEventRecord> output_events;
    bool starts_with_seed = false;
    bool starts_after_source_discontinuity = false;
    bool operator==(const PointerTrackSegmentRecord&) const = default;
};
using PointerTrackSegments = std::vector<PointerTrackSegmentRecord>;
struct PointerClickPhase {
    double progress = 0;
    double elapsed = 0;
    double duration = 0;
    PointerEventKind kind = PointerEventKind::kLeftClick;
    bool operator==(const PointerClickPhase&) const = default;
};
struct PointerSample {
    NormalizedPoint location{};
    bool is_clicking = false;
    std::optional<PointerClickPhase> click_phase;
    double rotation_radians = 0;
    std::optional<CursorAssetID> recorded_cursor_asset_id;
};
struct PointerTrackEvaluation {
    std::optional<NormalizedPoint> position;
    std::optional<PointerSample> cursor;
};

/// 构造时完成平滑轨迹；求值只读且可安全跨线程使用。
/// 修改弹簧参数时创建新轨道，避免逐帧重建或隐式共享可变缓存。
class PointerTrack {
public:
    explicit PointerTrack(std::vector<PointerEventRecord> events);
    PointerTrack(std::vector<PointerEventRecord> events, const MotionStyle& motion);
    [[nodiscard]] const std::vector<PointerEventRecord>& events() const { return events_; }
    [[nodiscard]] PointerTrackEvaluation evaluation(TimeInterval time, const MotionStyle& motion,
                                                     const CursorStyle& style) const;
    [[nodiscard]] std::optional<NormalizedPoint> position(TimeInterval time,
                                                        const MotionStyle& motion) const;
    [[nodiscard]] size_t cached_spring_variant_count() const;
private:
    struct SpringSample { TimeInterval time; NormalizedPoint location; };
    struct SpringVariant {
        double mass = 0, stiffness = 0, damping = 0;
        uint64_t last_used = 0;
        std::vector<SpringSample> samples;
    };
    struct SpringCache {
        std::mutex mutex;
        std::vector<SpringVariant> variants;
        uint64_t clock = 0;
    };
    std::vector<PointerEventRecord> events_;
    std::shared_ptr<SpringCache> spring_cache_;
    [[nodiscard]] size_t upper_index(TimeInterval time) const;
    [[nodiscard]] NormalizedPoint interpolated(size_t lower, size_t upper, TimeInterval time,
                                               const MotionStyle& motion) const;
    [[nodiscard]] std::optional<NormalizedPoint> resolved(TimeInterval time,
                                                        const MotionStyle& motion) const;
    [[nodiscard]] NormalizedPoint spring_position(TimeInterval time, const MotionStyle& motion) const;
    [[nodiscard]] std::vector<SpringSample> build_spring(const MotionStyle& motion) const;
    [[nodiscard]] NormalizedPoint sample_spring(const std::vector<SpringSample>& samples,
                                                 TimeInterval time) const;
};


/// 主时间线感知指针轨迹；每个 retained segment 独立插值，cut seed 与桥接不跨源段重放。
class ProjectPointerTrack {
public:
    explicit ProjectPointerTrack(PointerTrackSegments segments);
    ProjectPointerTrack(PointerTrackSegments segments, const MotionStyle& motion);
    [[nodiscard]] static ProjectPointerTrack from_timeline(
        const TimelineMap& timeline, const std::vector<PointerEventRecord>& source_events,
        const MotionStyle& motion);
    [[nodiscard]] PointerTrackEvaluation evaluation(TimeInterval output_time,
        const MotionStyle& motion, const CursorStyle& style, const ZoomClip* automatic_entry = nullptr) const;
    [[nodiscard]] std::optional<NormalizedPoint> position(TimeInterval output_time,
        const MotionStyle& motion) const;
    [[nodiscard]] bool automatic_entry_starts_after_cut(const ZoomClip& clip) const;
    [[nodiscard]] NormalizedPoint automatic_camera_focus(const ZoomClip& clip,
        TimeInterval output_time, const MotionStyle& motion, bool reanchor_entry = false) const;
    [[nodiscard]] const PointerTrackSegments& segments() const { return segment_records_; }
private:
    struct Segment {
        PointerTrackSegmentRecord record;
        PointerTrack track;
        std::optional<TimeInterval> first_click_time;
    };
    struct FocusSample { TimeInterval time = 0; NormalizedPoint position{}; };
    struct FocusKey {
        Uuid clip_id{};
        double start = 0, end = 0, scale = 0;
        NormalizedPoint focus{};
        double enter_duration = 0, enter_progress_offset = 0;
        double mass = 0, stiffness = 0, damping = 0;
        bool reanchor = false;
        bool operator==(const FocusKey&) const = default;
    };
    struct FocusVariant {
        FocusKey key{};
        std::uint64_t last_used = 0;
        std::vector<FocusSample> samples;
    };
    struct FocusCache {
        std::mutex mutex;
        std::uint64_t clock = 0;
        std::vector<FocusVariant> variants;
    };
    std::shared_ptr<FocusCache> focus_cache_ = std::make_shared<FocusCache>();
    [[nodiscard]] std::vector<FocusSample> build_automatic_focus(
        const ZoomClip&, const MotionStyle&, bool reanchor_entry) const;
    std::vector<PointerTrackSegmentRecord> segment_records_;
    std::vector<Segment> segments_;
    std::vector<TimeInterval> activity_times_;
    [[nodiscard]] std::optional<size_t> segment_index(TimeInterval output_time) const;
    [[nodiscard]] std::optional<NormalizedPoint> raw_position(TimeInterval output_time,
                                                               size_t segment_index) const;
};
}  // namespace dogsc
