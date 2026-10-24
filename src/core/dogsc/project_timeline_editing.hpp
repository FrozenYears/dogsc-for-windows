#pragma once

#include <optional>
#include <variant>

#include "dogsc/project_model.hpp"

namespace dogsc {

enum class RecordingSegmentTrimEdge { kLeft, kRight };
enum class ProjectTimelineTrack { kPrimaryRecording, kZoom, kScreenMotion, kCameraMotion, kMosaic, kSticker };
enum class ProjectTimelineEditingErrorCode {
    kInvalidSourceSequence, kSegmentNotFound, kSplitOutsideSegment, kDuplicateSegmentID,
    kTrimOutsideSegment, kCannotRemoveFinalSegment, kRestoreJunctionNotFound,
    kNoRestorableSourceGap, kNoRestorableLeadingSourceGap, kNoRestorableTrailingSourceGap,
    kCannotMergeDeletedGap, kCannotMergeDifferentPlaybackRates,
    kCannotMergeDifferentAudioOverrides, kInvalidPrimarySegmentAudioOverrides,
    kDuplicateClipID, kMissingClip, kMismatchedClipID, kInvalidClip, kOverlappingClips,
};

struct ProjectTimelineEditingError {
    ProjectTimelineEditingErrorCode code = ProjectTimelineEditingErrorCode::kInvalidSourceSequence;
    ProjectTimelineTrack track = ProjectTimelineTrack::kPrimaryRecording;
    Uuid id{};
    Uuid related_id{};
    constexpr bool operator==(const ProjectTimelineEditingError&) const = default;
};

using ProjectTimelineEditResult = std::variant<RecorderProject, ProjectTimelineEditingError>;

/// 主时间线的纯编辑变换；失败时返回可定位错误，不会修改输入工程。
class ProjectTimelineEditing {
public:
    static constexpr TimeInterval kEpsilon = 1.0 / 120000.0;

    static PrimarySegmentAudioOverrides primary_segment_audio_overrides(const RecorderProject&, const Uuid&);
    static ProjectTimelineEditResult setting_primary_segment_audio_overrides(
        const RecorderProject&, const Uuid&, const PrimarySegmentAudioOverrides&);
    static ProjectTimelineEditResult materialized(const RecorderProject&, TimeInterval full_source_duration);
    static ProjectTimelineEditResult repairing_persisted_ripple_transitions(
        const RecorderProject&, TimeInterval default_transition_duration = 0.7);
    static ProjectTimelineEditResult split_primary_segment(
        const RecorderProject&, TimeInterval output_time, const Uuid& new_right_segment_id,
        TimeInterval full_source_duration);
    static ProjectTimelineEditResult trim_primary_segment(
        const RecorderProject&, const Uuid&, RecordingSegmentTrimEdge, TimeInterval output_time,
        TimeInterval full_source_duration, TimeInterval default_transition_duration = 0.7);
    static ProjectTimelineEditResult remove_primary_segment(
        const RecorderProject&, const Uuid&, TimeInterval full_source_duration,
        TimeInterval default_transition_duration = 0.7);
    static ProjectTimelineEditResult move_primary_segment(
        const RecorderProject&, const Uuid&, int requested_index, TimeInterval full_source_duration);
    static ProjectTimelineEditResult restore_primary_gap(
        const RecorderProject&, const Uuid& previous_segment_id, const Uuid& next_segment_id,
        const Uuid& restored_segment_id, TimeInterval full_source_duration);
    static ProjectTimelineEditResult restore_primary_leading_gap(
        const RecorderProject&, const Uuid& restored_segment_id, TimeInterval full_source_duration);
    static ProjectTimelineEditResult restore_primary_trailing_gap(
        const RecorderProject&, const Uuid& restored_segment_id, TimeInterval full_source_duration);
    static ProjectTimelineEditResult merge_adjacent_primary_segments(
        const RecorderProject&, const Uuid& previous_segment_id, const Uuid& next_segment_id,
        TimeInterval full_source_duration);
    static ProjectTimelineEditResult setting_playback_rate(
        const RecorderProject&, const Uuid&, double requested_rate, TimeInterval full_source_duration);

    static ProjectTimelineEditResult inserting_zoom(const RecorderProject&, const ZoomClip&);
    static ProjectTimelineEditResult removing_zoom(const RecorderProject&, const Uuid&, TimeInterval default_return = 0.7);
    static ProjectTimelineEditResult replacing_zoom(const RecorderProject&, const Uuid&, const ZoomClip&);
    static ProjectTimelineEditResult inserting_screen_motion(const RecorderProject&, const ScreenMotionClip&);
    static ProjectTimelineEditResult removing_screen_motion(const RecorderProject&, const Uuid&, TimeInterval default_return = 0.7);
    static ProjectTimelineEditResult replacing_screen_motion(const RecorderProject&, const Uuid&, const ScreenMotionClip&);
    static ProjectTimelineEditResult inserting_camera_motion(const RecorderProject&, const CameraMotionClip&);
    static ProjectTimelineEditResult removing_camera_motion(const RecorderProject&, const Uuid&, TimeInterval default_return = 0.7);
    static ProjectTimelineEditResult replacing_camera_motion(const RecorderProject&, const Uuid&, const CameraMotionClip&);
    static ProjectTimelineEditResult inserting_mosaic(const RecorderProject&, const MosaicClip&);
    static ProjectTimelineEditResult removing_mosaic(const RecorderProject&, const Uuid&);
    static ProjectTimelineEditResult replacing_mosaic(const RecorderProject&, const Uuid&, const MosaicClip&);
    static ProjectTimelineEditResult inserting_sticker(const RecorderProject&, const StickerClip&);
    static ProjectTimelineEditResult removing_sticker(const RecorderProject&, const Uuid&);
    static ProjectTimelineEditResult replacing_sticker(const RecorderProject&, const Uuid&, const StickerClip&);

    static std::optional<ProjectTimelineEditingError> validate(const RecorderProject&);
    static std::optional<ProjectTimelineEditingError> validate(const RecorderProject&, TimeInterval full_source_duration);
};

}  // namespace dogsc
