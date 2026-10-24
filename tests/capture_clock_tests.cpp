#include <cstdio>
#include "dogsc/recording_clock.hpp"
#include "dogsc/project_json.hpp"
#include "test_framework.hpp"

namespace {
void pause_and_late_audio_use_original_host_spans() {
    dogsc::capture::RecordingClock clock;
    clock.start(10000000);
    clock.pause(20000000);
    clock.resume(40000000);
    clock.stop(50000000);
    EXPECT_EQ(*clock.map(10000000), 0);
    EXPECT_FALSE(clock.map(9999999).has_value());
    EXPECT_FALSE(clock.map(20000000).has_value());
    EXPECT_FALSE(clock.map(39999999).has_value());
    EXPECT_EQ(*clock.map(40000000), 10000000);
    EXPECT_FALSE(clock.map(50000000).has_value());
    EXPECT_EQ(clock.elapsed(80000000), 20000000);
    // 延迟交付的一整包音频同时跨越开始、暂停、恢复、停止边界。
    const auto slices = clock.slice_audio(5000000, 240000, 48000);
    EXPECT_EQ(slices.size(), 2u);
    EXPECT_EQ(slices[0].first_frame, 24000u);
    EXPECT_EQ(slices[0].frame_count, 48000u);
    EXPECT_EQ(slices[0].output_time, 0);
    EXPECT_EQ(slices[1].first_frame, 168000u);
    EXPECT_EQ(slices[1].frame_count, 48000u);
    EXPECT_EQ(slices[1].output_time, 10000000);
}
void audio_samples_never_cross_subsample_boundaries() {
    dogsc::capture::RecordingClock clock;
    clock.start(1);
    clock.stop(9999);
    const auto slices = clock.slice_audio(0, 100, 48000);
    EXPECT_EQ(slices.size(), 1u);
    EXPECT_EQ(slices[0].first_frame, 1u);
    EXPECT_EQ(slices[0].frame_count, 46u);
    EXPECT_EQ(slices[0].output_time, 207);
    EXPECT_TRUE(clock.slice_audio(10000, 100, 48000).empty());
}
void stopping_while_paused_does_not_append_pause() {
    dogsc::capture::RecordingClock clock;
    clock.start(1000);
    clock.pause(2000);
    clock.stop(5000);
    EXPECT_EQ(clock.elapsed(8000), 1000);
    EXPECT_FALSE(clock.map(3000).has_value());
    bool rejected = false;
    try { clock.resume(6000); } catch (const std::logic_error&) { rejected = true; }
    EXPECT_TRUE(rejected);
}
void project_media_roundtrip_preserves_alignment_and_inventory() {
    const std::string input = R"({"version":27,"media":{
        "screen":{"relativePath":"screen.mp4","sourceEndTime":2.5},
        "microphone":{"relativePath":"microphone.m4a","startOffset":0.02,
            "sourceStartTime":0.1,"sourceEndTime":2.4,"sourceTimeScale":1.001,
            "syncAnchors":[{"id":"00000000-0000-4000-8000-000000000002","sourceTime":2,"offset":0.01}]},
        "pointerEvents":{"relativePath":"pointer.jsonl"}}})";
    dogsc::RecorderProject project;
    dogsc::ProjectJsonError error;
    EXPECT_TRUE(dogsc::ProjectJson::decode(input, project, error));
    EXPECT_TRUE(project.media.has_value());
    EXPECT_EQ(project.media->screen.relative_path, "screen.mp4");
    EXPECT_FALSE(project.media->camera.has_value());
    EXPECT_NEAR(project.media->microphone->start_offset, 0.02, 1e-12);
    EXPECT_NEAR(*project.media->microphone->source_time_scale, 1.001, 1e-12);
    EXPECT_EQ(project.media->pointer_events->relative_path, "pointer.jsonl");
    dogsc::RecorderProject restored;
    EXPECT_TRUE(dogsc::ProjectJson::decode(dogsc::ProjectJson::encode(project), restored, error));
    EXPECT_EQ(restored, project);
    const auto old_project = restored;
    EXPECT_FALSE(dogsc::ProjectJson::decode(R"({"version":27,"media":{"microphone":{"relativePath":"orphan.m4a"}}})", restored, error));
    EXPECT_EQ(error.path, "/media/screen");
    EXPECT_EQ(restored, old_project);
}
}
int main() {
    pause_and_late_audio_use_original_host_spans();
    audio_samples_never_cross_subsample_boundaries();
    stopping_while_paused_does_not_append_pause();
    project_media_roundtrip_preserves_alignment_and_inventory();
    if (dogsc::test::failure_count()) return 1;
    std::puts("capture clock and media manifest checks passed");
    return 0;
}
