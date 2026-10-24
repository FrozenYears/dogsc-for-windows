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

void test_project_json_capture_configuration_round_trip() {
    using namespace dogsc;
    RecorderProject project;
    CaptureConfiguration& capture = project.capture;
    capture.source = CaptureSource::kDevice;
    capture.display_id = 0x12345678u;
    capture.display_name = "主显示器";
    capture.window_id = 0x87654321u;
    capture.window_name = "测试窗口";
    capture.selected_application_bundle_identifier = "com.example.app";
    capture.selected_application_name = "示例应用";
    capture.area = NormalizedRect{0.1, 0.2, 0.6, 0.7};
    capture.device_id = "device-1";
    capture.device_name = "设备一";
    capture.capture_frame_rate = OutputFrameRate::fps120;
    capture.capture_codec = CaptureCodec::kProRes422;
    capture.capture_resolution_limit = CaptureResolutionLimit::kFullHD;
    capture.hides_desktop_files = true;
    capture.hides_dock = true;
    capture.records_system_audio = false;
    capture.system_audio_scope = SystemAudioScope::kSelectedApplication;
    capture.records_microphone = true;
    capture.microphone_device_id = "microphone-1";
    capture.microphone_device_name = "麦克风一";
    capture.records_camera = true;
    capture.camera_device_id = "camera-1";
    capture.camera_device_name = "摄像头一";
    capture.camera_capture_resolution = CameraCaptureResolution{1920, 1080};

    const std::string encoded = ProjectJson::encode(project);
    for (const char* key : {"capture", "source", "displayID", "displayName", "windowID",
                            "windowName", "selectedApplicationBundleIdentifier",
                            "selectedApplicationName", "area", "deviceID", "deviceName",
                            "captureFrameRate", "captureCodec", "captureResolutionLimit",
                            "hidesDesktopFiles", "hidesDock", "recordsSystemAudio",
                            "systemAudioScope", "recordsMicrophone", "microphoneDeviceID",
                            "microphoneDeviceName", "recordsCamera", "cameraDeviceID",
                            "cameraDeviceName", "cameraCaptureResolution"}) {
        EXPECT_TRUE(encoded.find(std::string("\"") + key + "\"") != std::string::npos);
    }

    RecorderProject decoded;
    ProjectJsonError error;
    EXPECT_TRUE(ProjectJson::decode(encoded, decoded, error));
    EXPECT_TRUE(decoded == project);
    EXPECT_EQ(ProjectJson::encode(decoded), encoded);
}

void test_capture_configuration_defaults_and_missing_json_fields() {
    using namespace dogsc;
    const CaptureConfiguration defaults;
    EXPECT_EQ(defaults.source, CaptureSource::kDisplay);
    EXPECT_EQ(defaults.capture_frame_rate, OutputFrameRate::fps60);
    EXPECT_EQ(defaults.capture_codec, CaptureCodec::kHevc);
    EXPECT_EQ(defaults.capture_resolution_limit, CaptureResolutionLimit::kNative);
    EXPECT_TRUE(defaults.records_system_audio);
    EXPECT_FALSE(defaults.records_microphone);
    EXPECT_FALSE(defaults.records_camera);

    RecorderProject project;
    ProjectJsonError error;
    EXPECT_TRUE(ProjectJson::decode(R"({"version":27})", project, error));
    EXPECT_EQ(project.capture, defaults);

    EXPECT_TRUE(ProjectJson::decode(R"({"version":27,"capture":{}})", project, error));
    EXPECT_EQ(project.capture.source, CaptureSource::kDisplay);
    EXPECT_EQ(project.capture.capture_frame_rate, OutputFrameRate::fps60);
    EXPECT_EQ(project.capture.capture_codec, CaptureCodec::kH264);
    EXPECT_EQ(project.capture.capture_resolution_limit, CaptureResolutionLimit::kNative);
    EXPECT_TRUE(project.capture.records_system_audio);
    EXPECT_FALSE(project.capture.records_microphone);
    EXPECT_FALSE(project.capture.records_camera);
    EXPECT_FALSE(project.capture.display_id.has_value());
    EXPECT_FALSE(project.capture.area.has_value());

    const NormalizedRect constrained = NormalizedRect{-0.5, 0.8, 2, 0}.constrained();
    EXPECT_NEAR(constrained.x, 0, 1e-12);
    EXPECT_NEAR(constrained.y, 0.8, 1e-12);
    EXPECT_NEAR(constrained.width, 1, 1e-12);
    EXPECT_NEAR(constrained.height, 0.02, 1e-12);
}

void test_capture_dimensions_preserve_even_sizes_and_orientation() {
    using namespace dogsc;
    const CaptureDimensions compatible = CaptureDimensions::h264_compatible(5001, 3001);
    EXPECT_EQ(compatible.width, 3838);
    EXPECT_EQ(compatible.height, 2304);
    EXPECT_EQ(compatible.width % 2, 0);
    EXPECT_EQ(compatible.height % 2, 0);

    const CaptureDimensions landscape = apply_capture_resolution_limit(
        CaptureResolutionLimit::kUHD, CaptureDimensions{5000, 3000});
    EXPECT_EQ(landscape.width, 3600);
    EXPECT_EQ(landscape.height, 2160);
    const CaptureDimensions portrait = apply_capture_resolution_limit(
        CaptureResolutionLimit::kUHD, CaptureDimensions{3000, 5000});
    EXPECT_EQ(portrait.width, 2160);
    EXPECT_EQ(portrait.height, 3600);
    EXPECT_EQ(apply_capture_resolution_limit(CaptureResolutionLimit::kNative,
                                             CaptureDimensions{321, 123}),
              (CaptureDimensions{321, 123}));
}

void test_project_json_timeline_tracks_round_trip() {
    using namespace dogsc;
    RecorderProject project;
    const Uuid segment_id = uuid_from_last_byte(20);
    PrimarySegmentAudioOverrides overrides;
    overrides.system_volume = 0.31;
    overrides.microphone_volume = 0.77;
    overrides.is_system_muted = false;
    overrides.is_microphone_muted = true;
    project.primary_segment_audio_overrides[segment_id] = overrides;

    FocusEffect focus_effect;
    focus_effect.shape = FocusEffectShape::kLinear;
    focus_effect.angle_degrees = 41;
    focus_effect.target = FocusEffectTarget::kPointer;
    focus_effect.center = {0.23, 0.78};
    focus_effect.size = 0.62;
    focus_effect.blur = 0.45;
    focus_effect.softness = 0.33;
    focus_effect.dimming = 0.12;

    ZoomClip zoom;
    zoom.id = uuid_from_last_byte(21);
    zoom.start_time = 1.2;
    zoom.end_time = 2.4;
    zoom.focus_effect = focus_effect;
    project.zoom_clips.push_back(zoom);

    ScreenMotionClip screen;
    screen.id = uuid_from_last_byte(22);
    screen.timing.start_time = 2.5;
    screen.timing.duration = 1.3;
    screen.timing.lead_in_duration = 0.4;
    screen.timing.return_duration = 0.2;
    screen.timing.preferred_lead_in_duration = 0.6;
    screen.timing.preferred_return_duration = 0.5;
    screen.timing.lead_in_progress_offset = 0.23;
    screen.timing.easing = ZoomEasingPreset::kQuintic;
    screen.timing.custom_curve = {0.21, 0.17, 0.82, 0.94};
    screen.timing.return_progress_offset = 0.61;
    screen.target.position = {0.37, 0.61};
    screen.target.scale = 1.4;
    screen.target.rotation_x = 12;
    screen.target.rotation_y = -7;
    screen.target.rotation_z = 3;
    screen.target.perspective = 0.72;
    screen.group_id = uuid_from_last_byte(23);
    screen.focus_effect = focus_effect;
    project.screen_motion_clips.push_back(screen);

    CameraMotionClip camera;
    camera.id = uuid_from_last_byte(24);
    camera.timing.start_time = 4;
    camera.timing.duration = 1.1;
    camera.timing.easing = ZoomEasingPreset::kSpring;
    camera.target.layout = CameraLayoutMode::shaped(CameraShape::kCircle);
    camera.target.position = {0.81, 0.72};
    camera.target.size = 0.42;
    camera.target.roundness = 0.9;
    camera.target.opacity = 0.83;
    camera.group_id = uuid_from_last_byte(25);
    project.camera_motion_clips.push_back(camera);

    CameraMotionClip fullscreen_camera;
    fullscreen_camera.id = uuid_from_last_byte(26);
    fullscreen_camera.target.layout = CameraLayoutMode::fullscreen();
    project.camera_motion_clips.push_back(fullscreen_camera);

    MosaicClip mosaic;
    mosaic.id = uuid_from_last_byte(27);
    mosaic.timing = {5.5, 2.2};
    mosaic.source_rect = {0.12, 0.24, 0.43, 0.32};
    mosaic.corner_radius = 0.14;
    mosaic.style = MosaicEffectStyle::kSpotlight;
    mosaic.intensity = 0.67;
    mosaic.spotlight_dimming = 0.31;
    mosaic.transition_style = MosaicTransitionStyle::kSmooth;
    mosaic.transition_in_duration = 0.45;
    mosaic.transition_out_duration = 0.52;
    project.mosaic_clips.push_back(mosaic);
    MosaicClip blur_mosaic;
    blur_mosaic.id = uuid_from_last_byte(29);
    blur_mosaic.timing = {7.8, 1.4};
    blur_mosaic.style = MosaicEffectStyle::kBlur;
    blur_mosaic.transition_style = MosaicTransitionStyle::kLinear;
    blur_mosaic.transition_in_duration = 0.16;
    blur_mosaic.transition_out_duration = 0.27;
    project.mosaic_clips.push_back(blur_mosaic);

    StickerClip sticker;
    sticker.id = uuid_from_last_byte(28);
    sticker.timing = {8, 3.5};
    sticker.relative_path = "Media/stickers/cat.png";
    sticker.position = {0.29, 0.68};
    sticker.width = 0.47;
    sticker.rotation_degrees = -14;
    sticker.opacity = 0.86;
    sticker.corner_radius = 13;
    sticker.border_width = 2.5;
    sticker.border_color = HexColor{"#3A7BCD"};
    sticker.shadow_opacity = 0.42;
    sticker.shadow_radius = 31;
    sticker.shadow_offset_x = -4;
    sticker.shadow_offset_y = 12;
    sticker.animation = StickerAnimationPreset::kSlideTopLeft;
    sticker.animation_curve = ElementMotionCurve::kGentle;
    sticker.exit_animation = StickerAnimationPreset::kFade;
    sticker.enter_duration = 0.83;
    sticker.exit_duration = 0.38;
    sticker.backdrop_blur = 17.5;
    sticker.backdrop_blur_includes_camera = true;
    sticker.hides_screen = true;
    sticker.hides_camera = true;
    sticker.layer_index = 3;
    project.sticker_clips.push_back(sticker);

    const std::string encoded = ProjectJson::encode(project);
    EXPECT_TRUE(encoded.find("\"primarySegmentAudioOverrides\"") != std::string::npos);
    EXPECT_TRUE(encoded.find("\"screenMotionClips\"") != std::string::npos);
    EXPECT_TRUE(encoded.find("\"cameraMotionClips\"") != std::string::npos);
    EXPECT_TRUE(encoded.find("\"mosaicClips\"") != std::string::npos);
    EXPECT_TRUE(encoded.find("\"stickerClips\"") != std::string::npos);

    RecorderProject decoded;
    ProjectJsonError error;
    EXPECT_TRUE(ProjectJson::decode(encoded, decoded, error));
    EXPECT_TRUE(decoded == project);
    EXPECT_EQ(ProjectJson::encode(decoded), encoded);

    EXPECT_TRUE((OverlayTiming{2, 3}.contains(4.99)));
    EXPECT_FALSE((OverlayTiming{2, 3}.contains(5)));
    EXPECT_TRUE(PrimarySegmentAudioOverrides{}.is_empty());
    EXPECT_TRUE(focus_effect.is_valid());
    FocusEffect invalid_focus = focus_effect;
    invalid_focus.size = 1.1;
    EXPECT_FALSE(invalid_focus.is_valid());
    EXPECT_EQ(sticker.automatic_exit(), StickerAnimationPreset::kSlideBottomRight);
    const NormalizedOverlayRect clamped = NormalizedOverlayRect{-0.5, 1.2, 2, 0}.clamped();
    EXPECT_NEAR(clamped.x, 0.0, 1e-12);
    EXPECT_NEAR(clamped.y, 0.99, 1e-12);
    EXPECT_NEAR(clamped.width, 1.0, 1e-12);
    EXPECT_NEAR(clamped.height, 0.01, 1e-12);
}

void test_project_json_timeline_missing_field_defaults() {
    using namespace dogsc;
    const std::string text = R"({
      "version": 27,
      "timeline": {
        "primarySegmentAudioOverrides": {
          "00000000-0000-4000-8000-00000000002A": {"systemVolume": 0.4}
        },
        "screenMotionClips": [{
          "id": "00000000-0000-4000-8000-00000000002B",
          "timing": {"startTime": 1, "duration": 2},
          "target": {"position": {"x": 0.5, "y": 0.5}, "scale": 1,
                      "rotationX": 0, "rotationY": 0, "rotationZ": 0, "perspective": 0.65}
        }],
        "cameraMotionClips": [{
          "id": "00000000-0000-4000-8000-00000000002C",
          "timing": {"startTime": 2, "duration": 1},
          "target": {"layout": {"kind": "shape", "shape": "方形"},
                     "position": {"x": 0.86, "y": 0.82}, "size": 0.35,
                     "roundness": 0.5, "opacity": 1}
        }],
        "mosaicClips": [{
          "id": "00000000-0000-4000-8000-00000000002D",
          "timing": {"startTime": 3, "duration": 4},
          "sourceRect": {"x": 0.1, "y": 0.2, "width": 0.3, "height": 0.4},
          "cornerRadius": 0.08, "style": "spotlight", "intensity": 0.6
        }],
        "stickerClips": [{
          "id": "00000000-0000-4000-8000-00000000002E",
          "timing": {"startTime": 4, "duration": 2},
          "relativePath": "Media/stickers/a.png", "position": {"x": 0.5, "y": 0.5},
          "width": 0.38, "rotationDegrees": 0, "opacity": 1, "cornerRadius": 18,
          "borderWidth": 0, "borderColor": "#FFFFFF", "shadowOpacity": 0.28,
          "shadowRadius": 24, "shadowOffsetX": 0, "shadowOffsetY": 10,
          "animation": "pop", "enterDuration": 0.7, "exitDuration": 0.22,
          "backdropBlur": 20, "layerIndex": 0
        }]
      }
    })";
    RecorderProject project;
    ProjectJsonError error;
    EXPECT_TRUE(ProjectJson::decode(text, project, error));
    EXPECT_EQ(project.primary_segment_audio_overrides.size(), 1U);
    EXPECT_FALSE(project.primary_segment_audio_overrides.begin()->second.microphone_volume);
    EXPECT_FALSE(project.screen_motion_clips[0].group_id.has_value());
    EXPECT_FALSE(project.screen_motion_clips[0].focus_effect.has_value());
    EXPECT_NEAR(project.screen_motion_clips[0].timing.lead_in_duration, 0.55, 1e-12);
    EXPECT_FALSE(project.camera_motion_clips[0].group_id.has_value());
    EXPECT_EQ(project.camera_motion_clips[0].target.layout.kind, CameraLayoutMode::Kind::kShape);
    EXPECT_NEAR(project.mosaic_clips[0].spotlight_dimming, 0.22, 1e-12);
    EXPECT_EQ(project.mosaic_clips[0].transition_style, MosaicTransitionStyle::kSmooth);
    EXPECT_NEAR(project.mosaic_clips[0].transition_in_duration, 0.30, 1e-12);
    EXPECT_EQ(project.sticker_clips[0].animation_curve, ElementMotionCurve::kSwift);
    EXPECT_FALSE(project.sticker_clips[0].exit_animation.has_value());
    EXPECT_FALSE(project.sticker_clips[0].backdrop_blur_includes_camera);
    EXPECT_FALSE(project.sticker_clips[0].hides_screen);
    EXPECT_FALSE(project.sticker_clips[0].hides_camera);
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
    EXPECT_TRUE(project.primary_segment_audio_overrides.empty());
    EXPECT_TRUE(project.screen_motion_clips.empty());
    EXPECT_TRUE(project.camera_motion_clips.empty());
    EXPECT_TRUE(project.mosaic_clips.empty());
    EXPECT_TRUE(project.sticker_clips.empty());
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
    EXPECT_NEAR(layout.width, 44.0, 1e-12);
    EXPECT_NEAR(layout.click_effect_width, 63.8, 1e-12);
    EXPECT_NEAR(layout.click_line_width, 3.52, 1e-12);
    metrics.hotspot_x = 0.5;
    metrics.hotspot_y = 0.5;
    EXPECT_TRUE(CursorRenderGeometry::layout(100, 200, 1080, 1.0, metrics, layout));
    EXPECT_NEAR(layout.origin_x, 100.0 - 22.0, 1e-12);
}

void test_cursor_assets_share_canonical_height_across_intrinsic_sizes() {
    using namespace dogsc;
    CursorAssetMetrics metrics;
    metrics.width = 22;
    metrics.height = 44;
    metrics.hotspot_x = 0.5;
    metrics.hotspot_y = 0.25;
    metrics.scale_reference_height = 44;
    CursorRenderLayout layout;
    EXPECT_TRUE(CursorRenderGeometry::layout(100, 200, 2160, 1.0, metrics, layout));
    EXPECT_NEAR(layout.height, 88, 1e-12);
    EXPECT_NEAR(layout.width, 44, 1e-12);
    EXPECT_NEAR(layout.origin_x, 78, 1e-12);
    EXPECT_NEAR(layout.origin_y, 178, 1e-12);
    EXPECT_NEAR(layout.click_effect_height, 88, 1e-12);
    EXPECT_NEAR(layout.click_effect_width, 127.6, 1e-12);
}


void test_cursor_click_geometry_preserves_all_effect_shapes() {
    using namespace dogsc;
    const auto ripple = CursorRenderGeometry::click_geometry(
        CursorClickEffectStyle::kRipple, 0, 44);
    EXPECT_TRUE(ripple.has_value());
    EXPECT_NEAR(ripple->primary_diameter, 44 * 0.45, 1e-12);
    EXPECT_NEAR(ripple->secondary_diameter, 44 * 0.28, 1e-12);
    EXPECT_TRUE(ripple->secondary_is_filled);
    EXPECT_NEAR(ripple->max_footprint_diameter, 44 * 2.2, 1e-12);

    const auto pulse = CursorRenderGeometry::click_geometry(
        CursorClickEffectStyle::kPulse, 0.55, 44);
    EXPECT_TRUE(pulse.has_value());
    EXPECT_NEAR(pulse->secondary_diameter, 44 * 0.25, 1e-9);

    const auto burst = CursorRenderGeometry::click_geometry(
        CursorClickEffectStyle::kBurst, 1, 44, 1, 1);
    EXPECT_TRUE(burst.has_value());
    EXPECT_NEAR(burst->primary_opacity, 0, 1e-12);
    EXPECT_NEAR(burst->accent_opacity, 0, 1e-12);
    EXPECT_NEAR(burst->max_footprint_diameter, 44 * 1.8, 1e-12);

    const auto glow_scaled = CursorRenderGeometry::click_geometry(
        CursorClickEffectStyle::kGlow, 0.5, 44, 2, 4);
    EXPECT_TRUE(glow_scaled.has_value());
    EXPECT_TRUE(glow_scaled->primary_is_filled);
    const double glow_fade = (0.5 - 0.18) / 0.82;
    EXPECT_NEAR(glow_scaled->primary_opacity,
                0.78 * (1 - std::pow(glow_fade, 1.4)), 1e-12);
    EXPECT_FALSE(CursorRenderGeometry::click_geometry(
        CursorClickEffectStyle::kNone, 0.5, 44).has_value());
    EXPECT_FALSE(CursorRenderGeometry::click_geometry(
        CursorClickEffectStyle::kRipple, std::nan(""), 44).has_value());
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
    test_project_json_capture_configuration_round_trip();
    test_capture_configuration_defaults_and_missing_json_fields();
    test_capture_dimensions_preserve_even_sizes_and_orientation();
    test_project_json_timeline_tracks_round_trip();
    test_project_json_timeline_missing_field_defaults();
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
    test_cursor_click_geometry_preserves_all_effect_shapes();
    test_cursor_assets_share_canonical_height_across_intrinsic_sizes();
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
