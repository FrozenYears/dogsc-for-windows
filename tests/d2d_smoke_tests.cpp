#include <array>
#include <cstdio>
#include <string>
#include <d3d11.h>
#include <wrl/client.h>
#include <vector>

#include "dogsc/d2d_frame_compositor.hpp"
#include "dogsc/project_model.hpp"
#include "test_framework.hpp"

namespace {
using namespace dogsc;

FrameRenderPlan pattern_plan() {
    FrameRenderPlan plan;
    plan.presentation_time = 0;
    plan.output_duration = 1;
    plan.frame_rate = 30;
    plan.scene.time = 0;
    plan.scene.canvas_size = {96, 64};
    plan.scene.color = FrameColorContract::sdr_desktop();
    plan.scene.background.source.kind = BackgroundSourceKind::kPattern;
    plan.scene.background.source.pattern_preset = "黑曜石网格";
    plan.scene.background.pattern_scale = 1;
    plan.scene.background.pattern_opacity = 1;
    plan.scene.layer_order = {FrameLayerRole::kBackground};
    return plan;
}

void warp_pattern_composition_is_repeatable_and_bgra_sized() {
    std::string error;
    auto compositor = D2DFrameCompositor::create_warp(error);
    EXPECT_TRUE(compositor != nullptr);
    if (!compositor) {
        std::fprintf(stderr, "WARP compositor unavailable: %s\n", error.c_str());
        return;
    }
    const auto plan = pattern_plan();
    const FrameRenderResources resources{};
    EXPECT_TRUE(compositor->compose(plan, resources));
    EXPECT_TRUE(compositor->last_error().empty());
    EXPECT_EQ(compositor->output_size(), (CompositionSize{96, 64}));

    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    EXPECT_TRUE(compositor->read_back(first));
    EXPECT_EQ(first.size(), std::size_t{96 * 64 * 4});
    EXPECT_TRUE(compositor->compose(plan, resources));
    EXPECT_TRUE(compositor->read_back(second));
    EXPECT_EQ(second, first);
}

void failed_layer_plan_does_not_publish_partial_output() {
    std::string error;
    auto compositor = D2DFrameCompositor::create_warp(error);
    EXPECT_TRUE(compositor != nullptr);
    if (!compositor) return;

    const auto plan = pattern_plan();
    const FrameRenderResources resources{};
    EXPECT_TRUE(compositor->compose(plan, resources));
    std::vector<std::uint8_t> published;
    EXPECT_TRUE(compositor->read_back(published));

    auto invalid = plan;
    invalid.scene.layer_order = {FrameLayerRole::kScreen, FrameLayerRole::kBackground};
    EXPECT_FALSE(compositor->compose(invalid, resources));
    EXPECT_FALSE(compositor->last_error().empty());
    std::vector<std::uint8_t> still_published;
    EXPECT_TRUE(compositor->read_back(still_published));
    EXPECT_EQ(still_published, published);
}
void warp_composes_screen_texture_and_hotspot_cursor() {
    std::string error;
    auto compositor = D2DFrameCompositor::create_warp(error);
    EXPECT_TRUE(compositor != nullptr);
    if (!compositor) return;

    D3D11_TEXTURE2D_DESC description{};
    description.Width = 32;
    description.Height = 32;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<std::uint32_t> red_pixels(32 * 32, 0xff0000ffU);
    D3D11_SUBRESOURCE_DATA initial{red_pixels.data(), 32 * 4, 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> screen;
    EXPECT_TRUE(SUCCEEDED(compositor->device()->CreateTexture2D(&description, &initial, &screen)));
    if (!screen) return;

    auto plan = pattern_plan();
    plan.scene.layer_order = {FrameLayerRole::kBackground, FrameLayerRole::kScreen};
    plan.scene.screen.source_crop = NormalizedCrop::full();
    plan.scene.screen.fitted_rect = {16, 16, 64, 32};
    plan.scene.screen.base_rect = plan.scene.screen.final_rect = {16, 16, 64, 32};
    plan.scene.screen.projected_quad = ScreenProjection::project(plan.scene.screen.final_rect, 0, 0, 0, 0);
    FrameRenderResources resources{};
    resources.screen = screen.Get();
    EXPECT_TRUE(compositor->compose(plan, resources));
    std::vector<std::uint8_t> pixels;
    EXPECT_TRUE(compositor->read_back(pixels));
    const auto pixel = [&pixels](unsigned x, unsigned y, unsigned channel) {
        return pixels[(static_cast<std::size_t>(y) * 96 + x) * 4 + channel];
    };
    EXPECT_TRUE(pixel(48, 32, 0) > pixel(48, 32, 1) * 3);
    EXPECT_TRUE(pixel(4, 4, 0) < 100 || pixel(4, 4, 1) < 100 || pixel(4, 4, 2) < 100);
}

void warp_composes_projected_screen_cursor_focus_camera_and_sticker_layers() {
    std::string error;
    auto compositor = D2DFrameCompositor::create_warp(error);
    EXPECT_TRUE(compositor != nullptr);
    if (!compositor) return;

    const auto make_texture = [&](std::uint32_t rgba, unsigned size) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = description.Height = size;
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size) * size, rgba);
        D3D11_SUBRESOURCE_DATA initial{pixels.data(), size * 4, 0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        EXPECT_TRUE(SUCCEEDED(compositor->device()->CreateTexture2D(&description, &initial, &texture)));
        return texture;
    };
    auto screen_texture = make_texture(0xff2020e0U, 32);
    auto camera_texture = make_texture(0xff30d030U, 24);
    auto cursor_texture = make_texture(0xff20f0f0U, 16);
    auto sticker_texture = make_texture(0xffffd020U, 16);
    if (!screen_texture || !camera_texture || !cursor_texture || !sticker_texture) return;

    auto plan = pattern_plan();
    plan.scene.time = 0.5;
    plan.scene.layer_order = {FrameLayerRole::kBackground, FrameLayerRole::kScreen,
        FrameLayerRole::kCursor, FrameLayerRole::kSpotlight, FrameLayerRole::kCamera,
        FrameLayerRole::kStickers};
    auto& screen = plan.scene.screen;
    screen.source_crop = NormalizedCrop::full();
    screen.fitted_rect = {8, 16, 70, 40};
    screen.base_rect = screen.final_rect = screen.fitted_rect;
    screen.projected_quad = ScreenProjection::project(screen.final_rect, 10, -15, 2, 0.4);
    screen.corner_radius = 5;
    screen.border_width = 2;
    screen.shadow = FrameShadow{HexColor::black(), 0.35, 3, {1, 2}};
    FrameCursorScene cursor;
    cursor.asset_id = CursorAssetID::system_arrow();
    cursor.metrics.width = 16;
    cursor.metrics.height = 16;
    cursor.metrics.hotspot_x = 0.5;
    cursor.metrics.hotspot_y = 0.5;
    cursor.metrics.scale_reference_height = 16;
    cursor.normalized_screen_position = {0.5, 0.5};
    EXPECT_TRUE(CursorRenderGeometry::layout(43, 36, 64, 1.0, cursor.metrics, cursor.layout));
    cursor.is_clicking = true;
    cursor.click_phase = PointerClickPhase{0.4, 0.1, 0.25, PointerEventKind::kLeftClick};
    cursor.click_style = CursorClickEffectStyle::kRipple;
    cursor.click_geometry = CursorRenderGeometry::click_geometry(cursor.click_style, 0.4,
        cursor.layout.click_effect_height);
    plan.scene.cursor = cursor;
    screen.mosaics.push_back({{0.2, 0.2, 0.3, 0.3}, 0.03,
        MosaicEffectStyle::kBlur, 0.35, 0.2, 1.0});
    FrameFocusScene focus;
    focus.effect.blur = 0.3;
    focus.effect.dimming = 0.1;
    focus.center = {0.5, 0.5};
    focus.progress = 0.5;
    screen.focus_effect = focus;
    plan.scene.camera = FrameCameraScene{
        {52, 32, 32, 24}, CameraContentFill::layout(24, 24, {52, 32, 32, 24}),
        12, 1.5, HexColor::white(), std::nullopt, 0.85, true};
    plan.scene.stickers.push_back({Uuid{}, "sticker.png", {0.5, 0.5}, 0.24, 0.1,
        0.92, 0.95, {}, 4, 1, HexColor::white(), 0.1, 2, {0, 1}, 3,
        true, false, false, 0.9, 2});
    FrameRenderResources resources{};
    resources.screen = screen_texture.Get();
    resources.camera = camera_texture.Get();
    resources.cursor = cursor_texture.Get();
    resources.stickers.emplace("sticker.png", sticker_texture.Get());
    EXPECT_TRUE(compositor->compose(plan, resources));
    if (!compositor->last_error().empty()) std::fprintf(stderr, "%s\n", compositor->last_error().data());
    std::vector<std::uint8_t> pixels;
    EXPECT_TRUE(compositor->read_back(pixels));
    EXPECT_EQ(pixels.size(), std::size_t{96 * 64 * 4});
    const auto pixel = [&](size_t x, size_t y) {
        const size_t offset = (y * 96 + x) * 4;
        return std::array<std::uint8_t, 4>{pixels[offset], pixels[offset + 1],
                                           pixels[offset + 2], pixels[offset + 3]};
    };
    const auto background = pixel(2, 2);
    const auto projected_screen = pixel(42, 23);
    const auto camera_layer = pixel(69, 42);
    const auto sticker_layer = pixel(48, 32);
    EXPECT_TRUE(background[3] > 250);
    EXPECT_TRUE(projected_screen[2] > 100);
    EXPECT_TRUE(camera_layer[1] > camera_layer[2] && camera_layer[1] > camera_layer[0]);
    EXPECT_TRUE(sticker_layer[2] > sticker_layer[0]);

}
}

int main() {
    warp_pattern_composition_is_repeatable_and_bgra_sized();
    failed_layer_plan_does_not_publish_partial_output();
    warp_composes_screen_texture_and_hotspot_cursor();
    warp_composes_projected_screen_cursor_focus_camera_and_sticker_layers();
    if (dogsc::test::failure_count() != 0) return 1;
    std::puts("D2D WARP compositor smoke passed");
    return 0;
}
