#include <cmath>
#include <cstdio>
#include <limits>

#include "dogsc/scene_geometry.hpp"
#include "test_framework.hpp"

namespace {
using dogsc::CameraContentFill;
using dogsc::CompositionPoint;
using dogsc::CompositionRect;
using dogsc::ScreenProjection;

void orthographic_projection_preserves_rect_corners() {
    const CompositionRect rect{20, 30, 640, 360};
    const auto quad = ScreenProjection::project(rect, 0, 0, 0, 0);
    EXPECT_EQ(quad.top_left, (CompositionPoint{20, 30}));
    EXPECT_EQ(quad.top_right, (CompositionPoint{660, 30}));
    EXPECT_EQ(quad.bottom_right, (CompositionPoint{660, 390}));
    EXPECT_EQ(quad.bottom_left, (CompositionPoint{20, 390}));
    EXPECT_TRUE(quad.contains({20, 30}));
    EXPECT_TRUE(quad.contains({320, 200}));
    EXPECT_FALSE(quad.contains({19.99, 200}));
}

void perspective_projector_roundtrips_points_outside_source_rect() {
    const CompositionRect rect{300, 150, 800, 450};
    const auto quad = ScreenProjection::project(rect, 17, -28, 6, 0.65,
                                                 CompositionPoint{420, 250});
    const CompositionPoint input{240, 228};
    const auto projected = quad.project(input, rect);
    EXPECT_TRUE(projected.has_value());
    const auto restored = quad.unproject(*projected, rect);
    EXPECT_TRUE(restored.has_value());
    EXPECT_NEAR(restored->x, input.x, 1e-8);
    EXPECT_NEAR(restored->y, input.y, 1e-8);
    EXPECT_TRUE(std::isfinite(quad.bounds().width));
}

void collapsed_projected_quad_rejects_inverse_mapping() {
    const dogsc::ProjectedScreenQuad quad{{1, 1}, {1, 1}, {1, 1}, {1, 1}};
    EXPECT_TRUE(quad.project({0, 0}, {0, 0, 10, 10}).has_value());
    EXPECT_FALSE(quad.unproject({1, 1}, {0, 0, 10, 10}).has_value());
    EXPECT_FALSE(quad.contains({1, 1}));
}

void camera_fill_respects_focus_crop_and_centered_mirror() {
    const auto focused = CameraContentFill::layout(1920, 1080, {100, 200, 400, 400},
                                                    {0.75, 0.25}, 1.5);
    EXPECT_TRUE(focused.has_value());
    EXPECT_NEAR(focused->target.mid_x(), 300, 1e-12);
    EXPECT_NEAR(focused->mirror_axis_x(), 300, 1e-12);
    EXPECT_TRUE(focused->visible_source.x > 0);
    EXPECT_TRUE(focused->visible_source.y > 0);

    const auto centered = CameraContentFill::layout(1920, 1080, {100, 200, 400, 400});
    EXPECT_TRUE(centered.has_value());
    const auto normal = centered->destination_point({1500, 250}, false);
    const auto mirrored = centered->destination_point({1500, 250}, true);
    EXPECT_NEAR(normal.x + mirrored.x, 2 * centered->mirror_axis_x(), 1e-8);
    EXPECT_NEAR(normal.y, mirrored.y, 1e-12);
}

void camera_fill_rejects_invalid_source_and_keeps_scale_bounded() {
    EXPECT_FALSE(CameraContentFill::layout(0, 720, {0, 0, 100, 100}).has_value());
    const auto layout = CameraContentFill::layout(640, 480, {0, 0, 320, 240},
        {std::numeric_limits<double>::quiet_NaN(), 0.5}, 9);
    EXPECT_TRUE(layout.has_value());
    EXPECT_NEAR(layout->scale, 0.5 * 1.01 * 4, 1e-12);
    EXPECT_NEAR(layout->visible_source.x, (640 - 320 / layout->scale) * 0.5, 1e-9);
}
void screen_frame_insets_match_authored_scale_and_fit_caps() {
    using dogsc::ScreenFrameGeometry;
    using dogsc::ScreenFrameStyle;
    EXPECT_EQ(dogsc::screen_frame_style_from_raw("unknown"), ScreenFrameStyle::kNone);
    EXPECT_EQ(ScreenFrameGeometry::decoration_insets_at_scale_one(
        ScreenFrameStyle::kNone, 1, 1, 1920, 1080, 1), (dogsc::ScreenDecorationInsets{}));
    const auto window = ScreenFrameGeometry::decoration_insets_at_scale_one(
        ScreenFrameStyle::kWindowLight, 1, 1, 1920, 1080, 1);
    EXPECT_NEAR(window.top, 46, 1e-12);
    EXPECT_NEAR(window.left, 0, 1e-12);
    const auto short_window = ScreenFrameGeometry::decoration_insets_at_scale_one(
        ScreenFrameStyle::kWindowLight, 1, 1, 80, 80, 1);
    EXPECT_NEAR(short_window.top, 20, 1e-12);
    const auto phone = ScreenFrameGeometry::decoration_insets_at_scale_one(
        ScreenFrameStyle::kDevicePhonePortrait, 1, 1, 400, 800, 1);
    EXPECT_NEAR(phone.left, 10.4, 1e-12);
    EXPECT_EQ(phone.left, phone.right);
}

}

int main() {
    orthographic_projection_preserves_rect_corners();
    perspective_projector_roundtrips_points_outside_source_rect();
    collapsed_projected_quad_rejects_inverse_mapping();
    camera_fill_respects_focus_crop_and_centered_mirror();
    camera_fill_rejects_invalid_source_and_keeps_scale_bounded();
    screen_frame_insets_match_authored_scale_and_fit_caps();
    if (dogsc::test::failure_count() != 0) return 1;
    std::puts("scene geometry checks passed");
    return 0;
}
