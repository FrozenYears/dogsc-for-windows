#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "dogsc/media_timeline.hpp"
#include "dogsc/project_json.hpp"
#include "dogsc/timing_guards.hpp"
#include "dogsc/output_frame_schedule.hpp"
#include "dogsc/timeline_map.hpp"
#include "dogsc/timeline_media_plan.hpp"
#include "test_framework.hpp"

namespace {

dogsc::Uuid uuid_from_last_byte(uint8_t last) {
    dogsc::Uuid id;
    id.bytes[6] = 0x40;
    id.bytes[8] = 0x80;
    id.bytes[15] = last;
    return id;
}

void test_frame_schedule_end_exclusive() {
    using dogsc::OutputFrameRate;
    using dogsc::OutputFrameSchedule;

    OutputFrameSchedule schedule;
    const auto error = OutputFrameSchedule::create(1.0, OutputFrameRate::fps60, schedule);
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(schedule.frame_count(), 60);
    EXPECT_EQ(schedule.ticks_per_frame(), 1500);

    // 端开区间：第 60 帧不存在，第 59 帧的时间戳正好是时长前一帧。
    EXPECT_FALSE(schedule.frame_at(60).has_value());
    const auto last = schedule.frame_at(59);
    EXPECT_TRUE(last.has_value());
    EXPECT_EQ(last->presentation_time_value, 59 * 1500);
    EXPECT_EQ(last->presentation_timescale, 90000);
}

void test_frame_schedule_snaps_representation_noise() {
    using dogsc::OutputFrameRate;
    using dogsc::OutputFrameSchedule;

    // (1/90)*90 在二进制浮点下不精确，但概念上是整 1 秒。
    const double duration = (1.0 / 90.0) * 90.0;
    OutputFrameSchedule schedule;
    const auto error = OutputFrameSchedule::create(duration, OutputFrameRate::fps90, schedule);
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(schedule.frame_count(), 90);
}

void test_frame_schedule_rejects_bad_input() {
    using dogsc::OutputFrameRate;
    using dogsc::OutputFrameSchedule;
    using dogsc::OutputFrameScheduleError;

    OutputFrameSchedule schedule;
    auto error = OutputFrameSchedule::create(-1.0, OutputFrameRate::fps60, schedule);
    EXPECT_TRUE(error.has_value());
    EXPECT_TRUE(*error == OutputFrameScheduleError::kInvalidDuration);

    error = OutputFrameSchedule::create(std::nan(""), OutputFrameRate::fps60, schedule);
    EXPECT_TRUE(error.has_value());
    EXPECT_TRUE(*error == OutputFrameScheduleError::kInvalidDuration);
}

void test_timeline_map_full_recording() {
    using namespace dogsc;
    TimelineMap map;
    const auto error = TimelineMap::create(SourceSequence::full_recording(), 10.0, map);
    EXPECT_FALSE(error.has_value());
    EXPECT_EQ(map.segments().size(), size_t{1});
    EXPECT_EQ(map.output_duration(), 10.0);
    EXPECT_TRUE(map.segments()[0].id == TimelineMap::full_recording_segment_id());

    const auto source = map.source_time_at_output_time(4.0);
    EXPECT_TRUE(source.has_value());
    EXPECT_NEAR(*source, 4.0, 1e-12);
}

void test_timeline_map_rejects_empty_edit() {
    using namespace dogsc;
    SourceSequence sequence;
    sequence.is_full_recording = false;
    TimelineMap map;
    const auto error = TimelineMap::create(sequence, 10.0, map);
    EXPECT_TRUE(error.has_value());
    EXPECT_TRUE(error->code == TimelineMapErrorCode::kEmptyEditedSequence);
}

void test_timeline_map_rejects_overlap() {
    using namespace dogsc;
    SourceSequence sequence;
    sequence.is_full_recording = false;
    sequence.segments.push_back(
        RecordingSegment{uuid_from_last_byte(1), 0.0, 6.0, 1.0});
    sequence.segments.push_back(
        RecordingSegment{uuid_from_last_byte(2), 5.0, 3.0, 1.0});
    TimelineMap map;
    const auto error = TimelineMap::create(sequence, 10.0, map);
    EXPECT_TRUE(error.has_value());
    EXPECT_TRUE(error->code == TimelineMapErrorCode::kNonMonotonicSegments);
}

void test_timeline_map_rejects_out_of_source() {
    using namespace dogsc;
    SourceSequence sequence;
    sequence.is_full_recording = false;
    sequence.segments.push_back(
        RecordingSegment{uuid_from_last_byte(3), 8.0, 5.0, 1.0});
    TimelineMap map;
    const auto error = TimelineMap::create(sequence, 10.0, map);
    EXPECT_TRUE(error.has_value());
    EXPECT_TRUE(error->code == TimelineMapErrorCode::kSegmentOutsideSource);
}

void test_timeline_map_playback_rate() {
    using namespace dogsc;
    SourceSequence sequence;
    sequence.is_full_recording = false;
    sequence.segments.push_back(RecordingSegment{uuid_from_last_byte(4), 0.0, 8.0, 2.0});
    TimelineMap map;
    const auto error = TimelineMap::create(sequence, 10.0, map);
    EXPECT_FALSE(error.has_value());
    // 8 秒源素材以 2× 播放，只需 4 秒输出时间。
    EXPECT_NEAR(map.output_duration(), 4.0, 1e-12);
    const auto source = map.source_time_at_output_time(2.0);
    EXPECT_TRUE(source.has_value());
    EXPECT_NEAR(*source, 4.0, 1e-12);
}

void test_media_plan_cut_boundary_is_seamless() {
    using namespace dogsc;
    SourceSequence sequence;
    sequence.is_full_recording = false;
    sequence.segments.push_back(RecordingSegment{uuid_from_last_byte(5), 0.0, 2.0, 1.0});
    sequence.segments.push_back(RecordingSegment{uuid_from_last_byte(6), 4.0, 2.0, 1.0});
    TimelineMap map;
    EXPECT_FALSE(TimelineMap::create(sequence, 10.0, map).has_value());

    const TimelineMediaPlan plan = TimelineMediaPlan::from_primary(map);
    EXPECT_EQ(plan.slices().size(), size_t{2});
    EXPECT_NEAR(plan.output_duration(), 4.0, 1e-12);

    // 切点本身必须归属后一段，且前后都不应出现空洞。
    const auto at_cut = plan.source_time_at_output_time(2.0);
    EXPECT_TRUE(at_cut.has_value());
    EXPECT_NEAR(*at_cut, 4.0, 1e-12);

    // 切点前一帧的最后一刻仍在第一段内。
    const auto before_cut = plan.source_time_at_output_time(2.0 - 1e-6);
    EXPECT_TRUE(before_cut.has_value());
    EXPECT_TRUE(*before_cut < 2.0);
}

void test_media_plan_contains_only_playable_ranges() {
    using namespace dogsc;
    TimelineMap map;
    EXPECT_FALSE(TimelineMap::create(SourceSequence::full_recording(), 5.0, map).has_value());
    const TimelineMediaPlan plan = TimelineMediaPlan::from_primary(map);
    EXPECT_TRUE(plan.contains(0.0));
    EXPECT_TRUE(plan.contains(4.999));
    EXPECT_FALSE(plan.contains(5.0));
    EXPECT_FALSE(plan.contains(-0.1));
}

void test_uuid_round_trip() {
    using namespace dogsc;
    const auto parsed = Uuid::parse("00000000-0000-4000-8000-000000000001");
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->to_string(), std::string("00000000-0000-4000-8000-000000000001"));
    EXPECT_TRUE(TimelineMap::full_recording_segment_id() == *parsed);

    EXPECT_FALSE(Uuid::parse("not-a-uuid").has_value());
    EXPECT_FALSE(Uuid::parse("00000000-0000-4000-8000").has_value());
}

void test_audio_anchored_auxiliary_plan() {
    using namespace dogsc;
    TimelineMap map;
    EXPECT_FALSE(TimelineMap::create(SourceSequence::full_recording(), 10.0, map).has_value());

    ProjectMediaReference reference;
    reference.relative_path = "system.caf";
    const MediaTimelinePlacement placement =
        MediaTimelinePlacement::make(reference, 0.0, 10.0, 10.0);
    const TimelineMediaPlan plan = TimelineMediaPlan::from_auxiliary(map, placement);
    EXPECT_EQ(plan.slices().size(), size_t{1});
    // 完全覆盖时直接复用主片段时长，不得因重算产生 ulp 级空洞。
    EXPECT_EQ(plan.slices()[0].duration, map.segments()[0].output_duration());
}

void test_sync_anchor_curve_extrapolates_from_zero() {
    using namespace dogsc;
    std::vector<MediaSyncAnchor> anchors;
    anchors.push_back(MediaSyncAnchor{uuid_from_last_byte(7), 2.0, 0.2});
    const MediaSyncAnchorCurve curve(anchors);
    EXPECT_NEAR(curve.offset_at_source_time(0.0), 0.0, 1e-12);
    EXPECT_NEAR(curve.offset_at_source_time(1.0), 0.1, 1e-12);
    EXPECT_NEAR(curve.offset_at_source_time(2.0), 0.2, 1e-12);
    // 末锚点之后保持最后一次校正。
    EXPECT_NEAR(curve.offset_at_source_time(9.0), 0.2, 1e-12);
}


void test_project_json_round_trip() {
    using namespace dogsc;
    RecorderProject project;
    project.title = "往返测试";
    project.created_at = "2026-10-23T12:00:00Z";
    project.canvas.padding = 128;
    project.canvas.background_source = BackgroundSource::safe_fallback();
    project.camera.position = NormalizedPoint{0.9, 0.4};
    project.cursor_style.asset_id = CursorAssetID::system_pointing_hand();
    project.opening_sequence.is_enabled = true;
    project.export_settings.frame_rate = 60;

    ZoomClip clip;
    clip.id = uuid_from_last_byte(9);
    clip.start_time = 1.5;
    clip.end_time = 3.0;
    clip.scale = 1.8;
    clip.origin = ZoomKeyframeOrigin::kAutomatic;
    clip.easing = ZoomEasingPreset::kSpring;
    project.zoom_clips.push_back(clip);

    project.recording_markers.push_back(
        RecordingMarker{uuid_from_last_byte(10), 2.5, 1});

    const std::string encoded = ProjectJson::encode(project);
    EXPECT_FALSE(encoded.empty());

    RecorderProject decoded;
    ProjectJsonError error;
    EXPECT_TRUE(ProjectJson::decode(encoded, decoded, error));
    EXPECT_EQ(error.code, ProjectJsonErrorCode::kParseFailure);
    EXPECT_TRUE(decoded == project);

    // 再写一次必须逐字节相同，证明编码是确定性的。
    EXPECT_EQ(ProjectJson::encode(decoded), encoded);
}

void test_project_json_missing_fields_use_defaults() {
    using namespace dogsc;
    const std::string minimal = R"({"version": 27, "title": "最小工程"})";
    RecorderProject project;
    ProjectJsonError error;
    EXPECT_TRUE(ProjectJson::decode(minimal, project, error));
    EXPECT_EQ(project.title, std::string("最小工程"));
    EXPECT_EQ(project.version, ProjectSchema::kCurrentVersion);
    EXPECT_NEAR(project.canvas.padding, 100.0, 1e-12);
    EXPECT_NEAR(project.audio.system_volume, 0.82, 1e-12);
    EXPECT_TRUE(project.source_sequence.is_full_recording);
}

void test_project_json_unknown_fields_ignored() {
    using namespace dogsc;
    const std::string text =
        R"({"version": 27, "title": "T", "unknownFutureField": {"a": 1}})";
    RecorderProject project;
    ProjectJsonError error;
    EXPECT_TRUE(ProjectJson::decode(text, project, error));
    EXPECT_FALSE(ProjectJson::encode(project).find("unknownFutureField") !=
                 std::string::npos);
}

void test_project_json_rejects_legacy_and_future_schema() {
    using namespace dogsc;
    RecorderProject project;
    ProjectJsonError error;
    EXPECT_FALSE(ProjectJson::decode(R"({"version": 22})", project, error));
    EXPECT_EQ(error.code, ProjectJsonErrorCode::kSchemaRejected);
    EXPECT_FALSE(ProjectJson::decode(R"({"version": 28})", project, error));
    EXPECT_EQ(error.code, ProjectJsonErrorCode::kSchemaRejected);
}

void test_project_json_rejects_malformed_and_non_object() {
    using namespace dogsc;
    RecorderProject project;
    ProjectJsonError error;
    EXPECT_FALSE(ProjectJson::decode("{ not json", project, error));
    EXPECT_EQ(error.code, ProjectJsonErrorCode::kParseFailure);
    EXPECT_FALSE(ProjectJson::decode("[1,2,3]", project, error));
    EXPECT_EQ(error.code, ProjectJsonErrorCode::kNotAnObject);
}

void test_project_json_writes_current_version() {
    using namespace dogsc;
    RecorderProject project;
    project.version = 23;
    const std::string encoded = ProjectJson::encode(project);
    EXPECT_TRUE(encoded.find("\"version\": 27") != std::string::npos);
}

void test_layer_order_validation() {
    using namespace dogsc;
    EXPECT_TRUE(is_valid_layer_order({FrameLayerRole::kBackground, FrameLayerRole::kScreen}));
    EXPECT_TRUE(is_valid_layer_order({FrameLayerRole::kBackground, FrameLayerRole::kScreen,
                                      FrameLayerRole::kCursor, FrameLayerRole::kSpotlight,
                                      FrameLayerRole::kCamera, FrameLayerRole::kStickers}));
    // 重排（光标画在屏幕之下）必须被拒绝。
    EXPECT_FALSE(is_valid_layer_order({FrameLayerRole::kBackground, FrameLayerRole::kCursor,
                                       FrameLayerRole::kScreen}));
    // 重复图层必须被拒绝。
    EXPECT_FALSE(is_valid_layer_order({FrameLayerRole::kBackground, FrameLayerRole::kScreen,
                                       FrameLayerRole::kScreen}));
}

void test_render_plan_layer_order() {
    using namespace dogsc;
    const FrameRenderPlan plan =
        FrameSceneEvaluator::render_plan(1.0, 10.0, 60, CompositionSize{1920, 1080}, true, true,
                                         true, true);
    EXPECT_TRUE(is_valid_layer_order(plan.scene.layer_order));
    EXPECT_EQ(plan.scene.layer_order.size(), size_t{6});

    const FrameRenderPlan bare = FrameSceneEvaluator::render_plan(
        1.0, 10.0, 60, CompositionSize{1920, 1080}, false, false, false, false);
    EXPECT_EQ(bare.scene.layer_order.size(), size_t{2});
    EXPECT_TRUE(is_valid_layer_order(bare.scene.layer_order));
}

void test_render_plan_clamps_time() {
    using namespace dogsc;
    const FrameRenderPlan plan = FrameSceneEvaluator::render_plan(
        99.0, 10.0, 60, CompositionSize{1920, 1080}, false, false, false, false);
    EXPECT_NEAR(plan.presentation_time, 10.0, 1e-12);
    EXPECT_NEAR(plan.scene.time, 10.0, 1e-12);
}

void test_opening_sample_is_identity_when_disabled() {
    using namespace dogsc;
    OpeningSequence sequence;
    sequence.is_enabled = false;
    const auto active = sequence.participating_elements(true, true);
    const OpeningSample sample = FrameSceneEvaluator::opening_sample(
        sequence, OpeningSequenceElement::kScreen, active, 0.1, 1920, 1080);
    EXPECT_NEAR(sample.opacity, 1.0, 1e-12);
    EXPECT_NEAR(sample.scale, 1.0, 1e-12);
    EXPECT_NEAR(sample.offset.x, 0.0, 1e-12);
}

void test_opening_roster_is_stable() {
    using namespace dogsc;
    OpeningSequence sequence;
    sequence.is_enabled = true;
    sequence.duration = 2.0;
    sequence.stagger = 0.16;
    // 没有开场贴图时贴图不得进入参与名单，否则后续元素时序会漂移。
    const auto without_sticker = sequence.participating_elements(true, false);
    EXPECT_EQ(without_sticker.size(), size_t{2});
    const auto without_camera = sequence.participating_elements(false, true);
    EXPECT_EQ(without_camera.size(), size_t{2});
}

void test_cursor_layout_anchors_hotspot() {
    using namespace dogsc;
    CursorAssetMetrics metrics;
    metrics.width = 44;
    metrics.height = 44;
    metrics.hotspot_x = 0;
    metrics.hotspot_y = 0;
    CursorRenderLayout layout;
    EXPECT_TRUE(CursorRenderGeometry::layout(100, 200, 1080, 1.0, metrics, layout));
    EXPECT_NEAR(layout.origin_x, 100.0, 1e-12);
    EXPECT_NEAR(layout.origin_y, 200.0, 1e-12);
    EXPECT_NEAR(layout.width, 1080.0, 1e-12);

    metrics.hotspot_x = 0.5;
    metrics.hotspot_y = 0.5;
    EXPECT_TRUE(CursorRenderGeometry::layout(100, 200, 1080, 1.0, metrics, layout));
    EXPECT_NEAR(layout.origin_x, 100.0 - 540.0, 1e-12);
}

void test_cursor_layout_rejects_bad_metrics() {
    using namespace dogsc;
    CursorAssetMetrics metrics;
    metrics.width = 0;
    metrics.height = 44;
    CursorRenderLayout layout;
    EXPECT_FALSE(CursorRenderGeometry::layout(0, 0, 1080, 1.0, metrics, layout));
}

void test_canvas_pixel_dimensions_source_resolution() {
    using namespace dogsc;
    CanvasStyle style;
    style.aspect_ratio = CanvasAspectRatio::kAdaptive;
    const CanvasDimensions size =
        style.pixel_dimensions(CanvasResolution::kSource, std::nullopt,
                               CanvasDimensions{3840, 2160});
    EXPECT_EQ(size.width, 3840);
    EXPECT_EQ(size.height, 2160);
}

void test_canvas_pixel_dimensions_presets_are_even() {
    using namespace dogsc;
    CanvasStyle style;
    style.aspect_ratio = CanvasAspectRatio::kPortrait;
    const CanvasDimensions size = style.pixel_dimensions(CanvasResolution::kFullHD);
    EXPECT_EQ(size.width % 2, 0);
    EXPECT_EQ(size.height % 2, 0);
    EXPECT_EQ(size.height, 1920);
}



void test_guard_first_frame_normalization_is_stable() {
    using namespace dogsc;
    const auto first = TimingGuards::normalize_frame_time(120.0, 120.0, 0.0);
    EXPECT_TRUE(first.has_value());
    EXPECT_NEAR(*first, 0.0, 1e-12);

    // 同一 session_start 下，后续帧只受累计暂停影响。
    const auto later = TimingGuards::normalize_frame_time(125.5, 120.0, 2.0);
    EXPECT_TRUE(later.has_value());
    EXPECT_NEAR(*later, 3.5, 1e-12);

    EXPECT_FALSE(TimingGuards::normalize_frame_time(std::nan(""), 120.0, 0.0).has_value());
    EXPECT_FALSE(TimingGuards::normalize_frame_time(120.0, 120.0, -1.0).has_value());
}

void test_guard_non_monotonic_pts_is_discarded() {
    using namespace dogsc;
    EXPECT_FALSE(TimingGuards::should_discard_non_monotonic(0.0, std::nullopt));
    EXPECT_FALSE(TimingGuards::should_discard_non_monotonic(1.0, 1.0));
    EXPECT_TRUE(TimingGuards::should_discard_non_monotonic(0.9, 1.0));
    EXPECT_TRUE(TimingGuards::should_discard_non_monotonic(std::nan(""), 1.0));
}

void test_guard_shared_stop_freezes_every_track() {
    using namespace dogsc;
    // 摄像头比屏幕晚启动 0.25s，但两者都必须截断在同一 host 边界。
    const auto camera_end = TimingGuards::source_time_at_shared_stop(10.25, 0.25);
    EXPECT_TRUE(camera_end.has_value());
    EXPECT_NEAR(*camera_end, 10.0, 1e-12);

    // host 时间早于该路起点属于非法输入，调用方应记为 0 而不是负时间。
    EXPECT_FALSE(TimingGuards::source_time_at_shared_stop(0.1, 0.25).has_value());
}

void test_guard_pause_interruption_counts_only_real_gaps() {
    using namespace dogsc;
    // 恰好一帧间隔属于正常节拍，不计入暂停。
    EXPECT_NEAR(TimingGuards::pause_interruption(1.0, 1.0 - 1.0 / 60.0, 60), 0.0, 1e-12);
    // 暂停 2 秒后恢复：扣除一帧后的差额才是真实中断。
    const double interruption = TimingGuards::pause_interruption(3.0, 1.0, 60);
    EXPECT_NEAR(interruption, 2.0 - 1.0 / 60.0, 1e-9);
}

}  // namespace

int main() {
    test_frame_schedule_end_exclusive();
    test_frame_schedule_snaps_representation_noise();
    test_frame_schedule_rejects_bad_input();
    test_timeline_map_full_recording();
    test_timeline_map_rejects_empty_edit();
    test_timeline_map_rejects_overlap();
    test_timeline_map_rejects_out_of_source();
    test_timeline_map_playback_rate();
    test_media_plan_cut_boundary_is_seamless();
    test_media_plan_contains_only_playable_ranges();
    test_uuid_round_trip();
    test_audio_anchored_auxiliary_plan();
    test_sync_anchor_curve_extrapolates_from_zero();
    test_project_json_round_trip();
    test_project_json_missing_fields_use_defaults();
    test_project_json_unknown_fields_ignored();
    test_project_json_rejects_legacy_and_future_schema();
    test_project_json_rejects_malformed_and_non_object();
    test_project_json_writes_current_version();
    test_layer_order_validation();
    test_render_plan_layer_order();
    test_render_plan_clamps_time();
    test_opening_sample_is_identity_when_disabled();
    test_opening_roster_is_stable();
    test_cursor_layout_anchors_hotspot();
    test_cursor_layout_rejects_bad_metrics();
    test_canvas_pixel_dimensions_source_resolution();
    test_canvas_pixel_dimensions_presets_are_even();
    test_guard_first_frame_normalization_is_stable();
    test_guard_non_monotonic_pts_is_discarded();
    test_guard_shared_stop_freezes_every_track();
    test_guard_pause_interruption_counts_only_real_gaps();

    if (dogsc::test::failure_count() != 0) {
        std::fprintf(stderr, "%d 项断言失败\n", dogsc::test::failure_count());
        return 1;
    }
    std::printf("全部通过\n");
    return 0;
}
