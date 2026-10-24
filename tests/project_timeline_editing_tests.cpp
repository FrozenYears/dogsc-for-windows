#include <variant>

#include "dogsc/project_timeline_editing.hpp"
#include "test_framework.hpp"

namespace {
using namespace dogsc;

Uuid id(uint8_t value) {
    Uuid result;
    result.bytes[6] = 0x40;
    result.bytes[8] = 0x80;
    result.bytes[15] = value;
    return result;
}

RecorderProject edited_project() {
    RecorderProject project;
    project.source_sequence.is_full_recording = false;
    project.source_sequence.segments = {
        {id(1), 0, 2, 1}, {id(2), 4, 2, 1}, {id(3), 7, 3, 1},
    };
    return project;
}

const RecorderProject& project(const ProjectTimelineEditResult& result) {
    EXPECT_TRUE(std::holds_alternative<RecorderProject>(result));
    return std::get<RecorderProject>(result);
}

void test_split_rejects_cut_boundaries() {
    const RecorderProject input = edited_project();
    auto result = ProjectTimelineEditing::split_primary_segment(input, 2, id(4), 10);
    EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(result));
    EXPECT_TRUE(std::get<ProjectTimelineEditingError>(result).code ==
                ProjectTimelineEditingErrorCode::kSplitOutsideSegment);
    EXPECT_TRUE(input.source_sequence.segments.size() == 3);
}

void test_trim_outward_restores_gap() {
    const RecorderProject input = edited_project();
    const auto result = ProjectTimelineEditing::trim_primary_segment(
        input, id(2), RecordingSegmentTrimEdge::kLeft, 1, 10);
    const auto& output = project(result);
    EXPECT_NEAR(output.source_sequence.segments[1].source_start, 3, 1e-12);
    EXPECT_NEAR(output.source_sequence.segments[1].source_duration, 3, 1e-12);
}

void test_ripple_moves_all_visual_tracks() {
    RecorderProject input = edited_project();
    input.zoom_clips.push_back({id(11), 5, 6});
    input.screen_motion_clips.push_back({id(12), {5, 1}});
    input.camera_motion_clips.push_back({id(13), {5, 1}});
    input.mosaic_clips.push_back({id(14), {5, 1}});
    StickerClip sticker;
    sticker.id = id(15);
    sticker.timing = {5, 1};
    sticker.relative_path = "sticker.png";
    input.sticker_clips.push_back(sticker);
    const auto result = ProjectTimelineEditing::remove_primary_segment(input, id(1), 10);
    const auto& output = project(result);
    EXPECT_NEAR(output.zoom_clips.front().start_time, 3, 1e-12);
    EXPECT_NEAR(output.screen_motion_clips.front().timing.start_time, 3, 1e-12);
    EXPECT_NEAR(output.camera_motion_clips.front().timing.start_time, 3, 1e-12);
    EXPECT_NEAR(output.mosaic_clips.front().timing.start_time, 3, 1e-12);
    EXPECT_NEAR(output.sticker_clips.front().timing.start_time, 3, 1e-12);
}

void test_move_remaps_all_visual_tracks() {
    RecorderProject input = edited_project();
    input.mosaic_clips.push_back({id(21), {0.5, 0.5}});
    StickerClip sticker;
    sticker.id = id(22);
    sticker.timing = {4.5, 0.5};
    sticker.relative_path = "sticker.png";
    input.sticker_clips.push_back(sticker);
    const auto result = ProjectTimelineEditing::move_primary_segment(input, id(1), 2, 10);
    const auto& output = project(result);
    EXPECT_NEAR(output.mosaic_clips.front().timing.start_time, 5.5, 1e-12);
    EXPECT_NEAR(output.sticker_clips.front().timing.start_time, 2.5, 1e-12);
}

void test_audio_override_merge_requires_compatibility() {
    RecorderProject input;
    input.source_sequence.is_full_recording = false;
    input.source_sequence.segments = {{id(1), 0, 2, 1}, {id(2), 2, 2, 1}};
    input.primary_segment_audio_overrides[id(1)].system_volume = 0.2;
    input.primary_segment_audio_overrides[id(2)].system_volume = 0.3;
    const auto result = ProjectTimelineEditing::merge_adjacent_primary_segments(input, id(1), id(2), 4);
    EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(result));
    EXPECT_TRUE(std::get<ProjectTimelineEditingError>(result).code ==
                ProjectTimelineEditingErrorCode::kCannotMergeDifferentAudioOverrides);
}

void test_validation_rejects_duplicate_missing_and_overlap() {
    RecorderProject input = edited_project();
    input.zoom_clips = {{id(30), 0, 2}, {id(30), 1, 3}};
    const auto validation = ProjectTimelineEditing::validate(input);
    EXPECT_TRUE(validation.has_value());
    EXPECT_TRUE(validation->code == ProjectTimelineEditingErrorCode::kDuplicateClipID);
    const auto missing = ProjectTimelineEditing::removing_mosaic(input, id(99));
    EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(missing));
    EXPECT_TRUE(std::get<ProjectTimelineEditingError>(missing).code ==
                ProjectTimelineEditingErrorCode::kMissingClip);
}

void test_materialize_full_recording() {
    RecorderProject input;
    const auto result = ProjectTimelineEditing::materialized(input, 12);
    const auto& output = project(result);
    EXPECT_FALSE(output.source_sequence.is_full_recording);
    EXPECT_TRUE(output.source_sequence.segments.size() == 1);
    EXPECT_NEAR(output.source_sequence.segments.front().source_duration, 12, 1e-12);
}
void test_ripple_restores_transition_phase_and_intent() {
    auto input=edited_project();
    input.zoom_clips={{id(40),1,5}};
    auto& z=input.zoom_clips.front();z.enter_duration=3;z.preferred_enter_duration=3;z.enter_progress_offset=0.2;z.exit_duration=1;z.exit_progress_offset=0.3;
    input.screen_motion_clips={{id(41),{1,4}}};
    input.screen_motion_clips.front().timing.lead_in_duration=3;
    input.screen_motion_clips.front().timing.lead_in_progress_offset=0.2;
    input.screen_motion_clips.front().timing.return_duration=1;
    input.camera_motion_clips={{id(42),input.screen_motion_clips.front().timing}};
    const auto before=input;
    const auto result=ProjectTimelineEditing::remove_primary_segment(input,id(2),10);
    const auto& out=project(result);
    EXPECT_NEAR(out.zoom_clips.front().enter_duration,2,1e-12);
    EXPECT_NEAR(out.zoom_clips.front().enter_progress_offset,0.2,1e-12);
    EXPECT_NEAR(out.zoom_clips.front().exit_duration,1,1e-12);
    EXPECT_NEAR(out.zoom_clips.front().exit_progress_offset,0.3,1e-12);
    EXPECT_NEAR(out.screen_motion_clips.front().timing.lead_in_duration,2,1e-12);
    EXPECT_NEAR(out.camera_motion_clips.front().timing.lead_in_progress_offset,0.2,1e-12);
    EXPECT_TRUE(input==before);
    const auto left=ProjectTimelineEditing::trim_primary_segment(input,id(1),RecordingSegmentTrimEdge::kLeft,1.5,10);
    EXPECT_NEAR(project(left).zoom_clips.front().enter_progress_offset,0,1e-12);
    EXPECT_NEAR(project(left).zoom_clips.front().enter_duration,3,1e-12);
    const auto tail=ProjectTimelineEditing::trim_primary_segment(input,id(3),RecordingSegmentTrimEdge::kRight,6,10);
    EXPECT_NEAR(project(tail).zoom_clips.front().end_time,input.zoom_clips.front().end_time,1e-12);
    EXPECT_NEAR(project(tail).zoom_clips.front().exit_progress_offset,0.3,1e-12);
}

void test_removed_successor_restores_motion_return() {
    auto input=edited_project();
    TransitionTiming timing;timing.start_time=0;timing.duration=1;timing.return_duration=0;timing.return_progress_offset=0.8;timing.preferred_return_duration=1.2;
    input.screen_motion_clips={{id(50),timing},{id(51),{1,1}}};
    input.camera_motion_clips={{id(52),timing},{id(53),{1,1}}};
    const auto screen=ProjectTimelineEditing::removing_screen_motion(input,id(51));
    EXPECT_NEAR(project(screen).screen_motion_clips.front().timing.return_duration,1.2,1e-12);
    EXPECT_NEAR(project(screen).screen_motion_clips.front().timing.return_progress_offset,0,1e-12);
    const auto camera=ProjectTimelineEditing::removing_camera_motion(input,id(53));
    EXPECT_NEAR(project(camera).camera_motion_clips.front().timing.return_duration,1.2,1e-12);
    const auto ripple=ProjectTimelineEditing::remove_primary_segment(input,id(1),10,0.9);
    EXPECT_TRUE(project(ripple).screen_motion_clips.empty());
}

void test_persisted_repairs_cover_zoom_and_respect_adjacency() {
    auto input=edited_project();input.zoom_clips={{id(60),0,2},{id(61),4,5}};
    input.zoom_clips[0].exit_duration=0;input.zoom_clips[0].exit_progress_offset=1;
    input.screen_motion_clips={{id(62),{0,2}}};input.screen_motion_clips[0].timing.return_progress_offset=1;
    input.camera_motion_clips={{id(63),{0,2}},{id(64),{2,1}}};input.camera_motion_clips[0].timing.return_progress_offset=1;
    const auto result=ProjectTimelineEditing::repairing_persisted_ripple_transitions(input,0.8);
    EXPECT_NEAR(project(result).zoom_clips[0].exit_duration,0.8,1e-12);
    EXPECT_NEAR(project(result).zoom_clips[0].exit_progress_offset,0,1e-12);
    EXPECT_NEAR(project(result).screen_motion_clips[0].timing.return_duration,0.8,1e-12);
    EXPECT_NEAR(project(result).camera_motion_clips[0].timing.return_progress_offset,1,1e-12);
}

void test_move_normalizes_cross_segment_effects() {
    auto input=edited_project();input.zoom_clips={{id(70),1,4.5},{id(71),5,6}};
    input.zoom_clips[0].exit_duration=0;
    input.screen_motion_clips={{id(72),{1,3.5}},{id(73),{5,1}}};
    input.camera_motion_clips={{id(74),{1,3.5}},{id(75),{5,1}}};
    const auto result=ProjectTimelineEditing::move_primary_segment(input,id(3),0,10);
    const auto& out=project(result);
    EXPECT_NEAR(out.zoom_clips.front().start_time,1,1e-12);
    EXPECT_NEAR(out.zoom_clips.back().end_time,7,1e-12);
    EXPECT_FALSE(ProjectTimelineEditing::validate(out,10).has_value());
}

void test_playback_rate_preserves_output_duration_and_all_anchors() {
    auto input=edited_project();input.zoom_clips={{id(80),1,2},{id(81),2,3}};
    input.screen_motion_clips={{id(82),{1,1}},{id(83),{2,1}}};
    input.camera_motion_clips={{id(84),{2,1}}};input.mosaic_clips={{id(85),{2,1}}};
    StickerClip sticker;sticker.id=id(86);sticker.relative_path="image.png";sticker.timing={2,1};input.sticker_clips={sticker};
    const auto before=input;const auto result=ProjectTimelineEditing::setting_playback_rate(input,id(1),2,10);const auto& out=project(result);
    EXPECT_NEAR(out.source_sequence.segments[0].playback_rate,2,1e-12);
    EXPECT_NEAR(out.zoom_clips[0].start_time,0.5,1e-12);
    EXPECT_NEAR(out.zoom_clips[0].duration(),1,1e-12);
    EXPECT_NEAR(out.zoom_clips[1].start_time,1.5,1e-12);
    EXPECT_NEAR(out.screen_motion_clips[1].timing.start_time,1.5,1e-12);
    EXPECT_NEAR(out.camera_motion_clips[0].timing.start_time,1,1e-12);
    EXPECT_NEAR(out.mosaic_clips[0].timing.start_time,1,1e-12);
    EXPECT_NEAR(out.sticker_clips[0].timing.start_time,1,1e-12);
    EXPECT_TRUE(input==before);
    const auto clamped=ProjectTimelineEditing::setting_playback_rate(input,id(1),1000,10);
    EXPECT_NEAR(project(clamped).source_sequence.segments[0].playback_rate,100,1e-12);
    const auto missing=ProjectTimelineEditing::setting_playback_rate(input,id(99),2,10);
    EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(missing));EXPECT_TRUE(input==before);
}

void test_all_track_replacements_move_trim_and_fail_atomically() {
    auto input=edited_project();ZoomClip z{id(90),0,1};ScreenMotionClip s{id(91),{0,1}};CameraMotionClip c{id(92),{0,1}};MosaicClip m{id(93),{0,1}};StickerClip t;t.id=id(94);t.relative_path="image.png";t.timing={0,1};
    input=project(ProjectTimelineEditing::inserting_zoom(input,z));input=project(ProjectTimelineEditing::inserting_screen_motion(input,s));input=project(ProjectTimelineEditing::inserting_camera_motion(input,c));input=project(ProjectTimelineEditing::inserting_mosaic(input,m));input=project(ProjectTimelineEditing::inserting_sticker(input,t));
    z.start_time=2;z.end_time=2.5;s.timing={2,0.5};c.timing={2,0.5};m.timing={2,0.5};t.timing={2,0.5};
    input=project(ProjectTimelineEditing::replacing_zoom(input,z.id,z));input=project(ProjectTimelineEditing::replacing_screen_motion(input,s.id,s));input=project(ProjectTimelineEditing::replacing_camera_motion(input,c.id,c));input=project(ProjectTimelineEditing::replacing_mosaic(input,m.id,m));input=project(ProjectTimelineEditing::replacing_sticker(input,t.id,t));
    EXPECT_NEAR(input.zoom_clips[0].start_time,2,1e-12);EXPECT_NEAR(input.screen_motion_clips[0].timing.duration,0.5,1e-12);EXPECT_NEAR(input.camera_motion_clips[0].timing.start_time,2,1e-12);EXPECT_NEAR(input.mosaic_clips[0].timing.duration,0.5,1e-12);EXPECT_NEAR(input.sticker_clips[0].timing.start_time,2,1e-12);
    const auto before=input;m.intensity=2;t.relative_path="assets/../image.png";z.enter_progress_offset=2;s.target.perspective=-1;c.target.opacity=2;
    for(const auto& failure:{ProjectTimelineEditing::replacing_zoom(input,z.id,z),ProjectTimelineEditing::replacing_screen_motion(input,s.id,s),ProjectTimelineEditing::replacing_camera_motion(input,c.id,c),ProjectTimelineEditing::replacing_mosaic(input,m.id,m),ProjectTimelineEditing::replacing_sticker(input,t.id,t)})EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(failure));
    EXPECT_TRUE(input==before);
    auto invalid=input;invalid.zoom_clips[0].exit_duration=6;const auto snapshot=invalid;
    EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(ProjectTimelineEditing::removing_mosaic(invalid,m.id)));EXPECT_TRUE(invalid==snapshot);
    input=project(ProjectTimelineEditing::removing_zoom(input,z.id));input=project(ProjectTimelineEditing::removing_screen_motion(input,s.id));input=project(ProjectTimelineEditing::removing_camera_motion(input,c.id));input=project(ProjectTimelineEditing::removing_mosaic(input,m.id));input=project(ProjectTimelineEditing::removing_sticker(input,t.id));
    EXPECT_TRUE(input.zoom_clips.empty()&&input.screen_motion_clips.empty()&&input.camera_motion_clips.empty()&&input.mosaic_clips.empty()&&input.sticker_clips.empty());
}

void test_outward_trim_moves_every_track_and_rejects_source_reuse() {
    auto input=edited_project();input.zoom_clips={{id(100),1,3}};input.screen_motion_clips={{id(101),{2,1}}};input.camera_motion_clips={{id(102),{2,1}}};input.mosaic_clips={{id(103),{1,2}}};StickerClip t;t.id=id(104);t.relative_path="image.png";t.timing={2,1};input.sticker_clips={t};
    const auto result=ProjectTimelineEditing::trim_primary_segment(input,id(2),RecordingSegmentTrimEdge::kLeft,1,10);const auto& out=project(result);
    EXPECT_NEAR(out.zoom_clips[0].end_time,4,1e-12);EXPECT_NEAR(out.screen_motion_clips[0].timing.start_time,3,1e-12);EXPECT_NEAR(out.camera_motion_clips[0].timing.start_time,3,1e-12);EXPECT_NEAR(out.mosaic_clips[0].timing.duration,3,1e-12);EXPECT_NEAR(out.sticker_clips[0].timing.start_time,3,1e-12);
    const auto before=input;const auto fail=ProjectTimelineEditing::trim_primary_segment(input,id(2),RecordingSegmentTrimEdge::kRight,6,10);EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(fail));EXPECT_TRUE(input==before);
}
void test_ripple_return_start_recovery_and_explicit_zero() {
    auto input=edited_project();input.zoom_clips={{id(110),0,3}};input.zoom_clips[0].exit_duration=1;input.zoom_clips[0].exit_progress_offset=0.6;
    TransitionTiming timing;timing.start_time=0;timing.duration=3;timing.lead_in_duration=0;timing.return_duration=1;timing.return_progress_offset=0.6;
    input.screen_motion_clips={{id(111),timing}};input.camera_motion_clips={{id(112),timing}};
    const auto result=ProjectTimelineEditing::remove_primary_segment(input,id(2),10,0.9);const auto& out=project(result);
    EXPECT_NEAR(out.zoom_clips[0].end_time,2,1e-12);EXPECT_NEAR(out.zoom_clips[0].exit_duration,1,1e-12);EXPECT_NEAR(out.zoom_clips[0].exit_progress_offset,0,1e-12);
    EXPECT_NEAR(out.screen_motion_clips[0].timing.return_duration,1,1e-12);EXPECT_NEAR(out.camera_motion_clips[0].timing.return_progress_offset,0,1e-12);
    input.screen_motion_clips[0].timing.return_duration=0;input.screen_motion_clips[0].timing.preferred_return_duration=0;
    input.camera_motion_clips[0].timing.return_duration=0;input.camera_motion_clips[0].timing.preferred_return_duration.reset();
    const auto zero=ProjectTimelineEditing::remove_primary_segment(input,id(2),10);
    EXPECT_NEAR(project(zero).screen_motion_clips[0].timing.return_duration,0,1e-12);EXPECT_NEAR(project(zero).camera_motion_clips[0].timing.return_duration,0,1e-12);
}

void test_gap_restore_split_merge_and_audio_operations() {
    auto input=edited_project();input.primary_segment_audio_overrides[id(1)].system_volume=0.4;input.mosaic_clips={{id(120),{1,2}}};
    const auto restored=ProjectTimelineEditing::restore_primary_gap(input,id(1),id(2),id(4),10);const auto& out=project(restored);
    EXPECT_NEAR(out.source_sequence.segments[1].source_start,2,1e-12);EXPECT_NEAR(out.source_sequence.segments[1].source_duration,2,1e-12);EXPECT_NEAR(out.mosaic_clips[0].timing.duration,4,1e-12);EXPECT_TRUE(out.primary_segment_audio_overrides.at(id(4))==input.primary_segment_audio_overrides.at(id(1)));
    const auto merged=ProjectTimelineEditing::merge_adjacent_primary_segments(out,id(1),id(4),10);EXPECT_NEAR(project(merged).source_sequence.segments[0].source_duration,4,1e-12);
    const auto split=ProjectTimelineEditing::split_primary_segment(input,1,id(5),10);EXPECT_TRUE(project(split).primary_segment_audio_overrides.at(id(5))==input.primary_segment_audio_overrides.at(id(1)));
    PrimarySegmentAudioOverrides overrides;overrides.microphone_volume=0.2;
    const auto audio=ProjectTimelineEditing::setting_primary_segment_audio_overrides(input,id(1),overrides);EXPECT_TRUE(ProjectTimelineEditing::primary_segment_audio_overrides(project(audio),id(1))==overrides);
    const auto cleared=ProjectTimelineEditing::setting_primary_segment_audio_overrides(project(audio),id(1),{});EXPECT_TRUE(project(cleared).primary_segment_audio_overrides.empty());
    auto trimmed=input;trimmed.source_sequence.segments={{id(1),2,4,2}};
    const auto leading=ProjectTimelineEditing::restore_primary_leading_gap(trimmed,id(6),10);EXPECT_NEAR(project(leading).source_sequence.segments[0].source_duration,2,1e-12);EXPECT_NEAR(project(leading).mosaic_clips[0].timing.start_time,3,1e-12);
    const auto trailing=ProjectTimelineEditing::restore_primary_trailing_gap(trimmed,id(7),10);EXPECT_NEAR(project(trailing).source_sequence.segments.back().source_duration,4,1e-12);EXPECT_TRUE(project(trailing).mosaic_clips==trimmed.mosaic_clips);
    const auto before=input;const auto fail=ProjectTimelineEditing::restore_primary_gap(input,id(1),id(3),id(8),10);EXPECT_TRUE(std::holds_alternative<ProjectTimelineEditingError>(fail));EXPECT_TRUE(input==before);
}
void test_validation_checks_transition_windows_and_targets() {
    auto input=edited_project();input.zoom_clips={{id(130),0,1},{id(131),1.2,2}};
    EXPECT_TRUE(ProjectTimelineEditing::validate(input)->code==ProjectTimelineEditingErrorCode::kOverlappingClips);
    input.zoom_clips.clear();input.screen_motion_clips={{id(132),{0,1}},{id(133),{1.2,1}}};input.screen_motion_clips[0].timing.return_duration=0.7;
    EXPECT_TRUE(ProjectTimelineEditing::validate(input)->code==ProjectTimelineEditingErrorCode::kOverlappingClips);
    input.screen_motion_clips.clear();input.camera_motion_clips={{id(134),{0,1}}};input.camera_motion_clips[0].timing.preferred_return_duration=6;
    EXPECT_TRUE(ProjectTimelineEditing::validate(input)->track==ProjectTimelineTrack::kCameraMotion);
    input.camera_motion_clips.clear();input.mosaic_clips={{id(135),{0,1}}};input.mosaic_clips[0].spotlight_dimming=0.8;
    EXPECT_TRUE(ProjectTimelineEditing::validate(input)->track==ProjectTimelineTrack::kMosaic);
    input.mosaic_clips.clear();StickerClip sticker;sticker.id=id(136);sticker.relative_path="/absolute.png";input.sticker_clips={sticker};
    EXPECT_TRUE(ProjectTimelineEditing::validate(input)->track==ProjectTimelineTrack::kSticker);
}
}  // namespace

int main() {
    test_split_rejects_cut_boundaries();
    test_trim_outward_restores_gap();
    test_ripple_moves_all_visual_tracks();
    test_move_remaps_all_visual_tracks();
    test_audio_override_merge_requires_compatibility();
    test_validation_rejects_duplicate_missing_and_overlap();
    test_materialize_full_recording();
    test_ripple_restores_transition_phase_and_intent();
    test_removed_successor_restores_motion_return();
    test_persisted_repairs_cover_zoom_and_respect_adjacency();
    test_move_normalizes_cross_segment_effects();
    test_playback_rate_preserves_output_duration_and_all_anchors();
    test_all_track_replacements_move_trim_and_fail_atomically();
    test_outward_trim_moves_every_track_and_rejects_source_reuse();
    test_ripple_return_start_recovery_and_explicit_zero();
    test_gap_restore_split_merge_and_audio_operations();
    test_validation_checks_transition_windows_and_targets();
    return dogsc::test::failure_count();
}
