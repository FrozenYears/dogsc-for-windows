#include <cmath>
#include <cstdio>
#include <vector>

#include "dogsc/project_model.hpp"
#include "dogsc/frame_scene_evaluator.hpp"
#include "dogsc/pointer_track.hpp"
#include "test_framework.hpp"

namespace {
using namespace dogsc;

void scene_contains_project_effects() {
    RecorderProject project;
    project.canvas.padding = 80;
    project.canvas.background_blur = 12;
    project.canvas.screen_frame = "browserDark";
    MosaicClip mosaic;
    mosaic.timing = {0.5, 1.5};
    mosaic.style = MosaicEffectStyle::kSpotlight;
    mosaic.transition_style = MosaicTransitionStyle::kSmooth;
    project.mosaic_clips.push_back(mosaic);
    StickerClip sticker;
    sticker.timing = {0.4, 1.6};
    sticker.animation = StickerAnimationPreset::kPop;
    sticker.relative_path = "sticker.png";
    project.sticker_clips.push_back(sticker);

    FrameEvaluationInputs inputs;
    inputs.screen_source_size = {1920, 1080};
    const auto scene = FrameSceneEvaluator::scene(project, 1.0, {1280, 720}, inputs, 2.0);
    EXPECT_EQ(scene.canvas_size, (CompositionSize{1280, 720}));
    EXPECT_EQ(scene.background.source.kind, project.canvas.background_source.kind);
    EXPECT_TRUE(scene.screen.decoration.has_value());
    EXPECT_EQ(scene.screen.mosaics.size(), size_t{1});
    EXPECT_EQ(scene.stickers.size(), size_t{1});
    EXPECT_TRUE(is_valid_layer_order(scene.layer_order));
}

void zoom_viewport_roundtrips_points() {
    ZoomSample sample;
    sample.scale = 2.4;
    sample.focus = {0.2, 0.75};
    sample.framing_progress = 1;
    const auto transform = ZoomViewportTransform::make(sample, NormalizedCrop::full());
    const NormalizedPoint source{0.37, 0.64};
    const auto mapped = transform.applying(source);
    const auto restored = transform.unapplying(mapped);
    EXPECT_NEAR(restored.x, source.x, 1e-12);
    EXPECT_NEAR(restored.y, source.y, 1e-12);
}

void pointer_segments_bridge_only_at_cut() {
    PointerTrackSegmentRecord first;
    first.output_range = {0, 1};
    first.output_events = {{0, {0.1, 0.2}, PointerEventKind::kMove},
                           {0.9, {0.2, 0.2}, PointerEventKind::kMove}};
    PointerTrackSegmentRecord second;
    second.output_range = {1, 1};
    second.starts_with_seed = true;
    second.starts_after_source_discontinuity = true;
    second.output_events = {{1, {0.8, 0.8}, PointerEventKind::kMove},
                            {1.5, {0.9, 0.8}, PointerEventKind::kMove}};
    second.output_range.duration = 1;
    ProjectPointerTrack track({first, second}, MotionStyle{});
    CursorStyle style;
    style.click_effect_style = CursorClickEffectStyle::kNone;
    MotionStyle motion;
    motion.cursor = CursorMotionStyle::kMedium;
    const auto before = track.position(0.95, motion);
    const auto at_cut = track.position(1.0, motion);
    const auto after = track.position(1.5, motion);
    EXPECT_TRUE(before.has_value());
    EXPECT_TRUE(at_cut.has_value());
    EXPECT_TRUE(after.has_value());
    EXPECT_NEAR(at_cut->x, 0.8, 1e-12);
    EXPECT_NEAR(after->x, 0.9, 1e-12);
}

void pointer_spring_cache_keeps_two_recent_parameter_variants() {
    std::vector<PointerEventRecord> events{
        {0, {0.1, 0.2}, PointerEventKind::kMove},
        {0.25, {0.4, 0.7}, PointerEventKind::kMove},
        {0.5, {0.8, 0.3}, PointerEventKind::kMove}};
    PointerTrack track(events, MotionStyle{});
    MotionStyle first;
    EXPECT_TRUE(track.position(0.3, first).has_value());
    EXPECT_EQ(track.cached_spring_variant_count(), size_t{1});
    MotionStyle second = first;
    second.cursor_spring_mass += 0.25;
    EXPECT_TRUE(track.position(0.3, second).has_value());
    MotionStyle third = first;
    third.cursor_spring_stiffness += 10;
    EXPECT_TRUE(track.position(0.3, third).has_value());
    EXPECT_EQ(track.cached_spring_variant_count(), size_t{2});
}

void scene_clamps_frame_request() {
    RecorderProject project;
    FrameEvaluationInputs inputs;
    inputs.screen_source_size = {1920, 1080};
    const auto plan = FrameSceneEvaluator::render_plan(project, 99, -1, 0, {1, 1}, inputs);
    EXPECT_EQ(plan.presentation_time, 0.0);
    EXPECT_EQ(plan.output_duration, 0.0);
    EXPECT_EQ(plan.frame_rate, 1);
    EXPECT_TRUE(is_valid_layer_order(plan.scene.layer_order));
}

}  // namespace

int main() {
    scene_contains_project_effects();
    zoom_viewport_roundtrips_points();
    pointer_segments_bridge_only_at_cut();
    pointer_spring_cache_keeps_two_recent_parameter_variants();
    scene_clamps_frame_request();
    if (dogsc::test::failure_count() != 0) return 1;
    std::puts("frame scene checks passed");
    return 0;
}
