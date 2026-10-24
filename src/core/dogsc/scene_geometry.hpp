#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "dogsc/basic_types.hpp"
#include "dogsc/composition_types.hpp"

namespace dogsc {

/// 屏幕四角映射到画布坐标；所有方法使用左上原点。
struct ProjectedScreenQuad {
    CompositionPoint top_left{};
    CompositionPoint top_right{};
    CompositionPoint bottom_right{};
    CompositionPoint bottom_left{};
    [[nodiscard]] std::array<CompositionPoint, 4> corners() const;

    constexpr bool operator==(const ProjectedScreenQuad&) const = default;
    [[nodiscard]] CompositionRect bounds() const;
    [[nodiscard]] CompositionPoint center() const;
    [[nodiscard]] CompositionPoint resize_handle_point() const { return bottom_right; }
    [[nodiscard]] std::optional<CompositionPoint> project(
        CompositionPoint point, CompositionRect source_rect) const;
    [[nodiscard]] std::optional<CompositionPoint> unproject(
        CompositionPoint point, CompositionRect source_rect) const;
    [[nodiscard]] double semantic_resize_delta(CompositionPoint translation,
                                               double reference_length) const;
    [[nodiscard]] bool contains(CompositionPoint point, double tolerance = 1e-6) const;
};

/// 确定性屏幕透视投影。perspective=0 为正交投影。
class ScreenProjection {
public:
    [[nodiscard]] static ProjectedScreenQuad project(
        CompositionRect rect, double rotation_x, double rotation_y,
        double rotation_z, double perspective,
        std::optional<CompositionPoint> anchor = std::nullopt);
};

/// 摄像头 aspect-fill 几何，镜像在目标 aperture 内应用。
struct CameraContentFillLayout {
    CompositionRect source{};
    CompositionRect target{};
    CompositionRect destination{};
    CompositionRect visible_source{};
    double scale = 1;

    constexpr bool operator==(const CameraContentFillLayout&) const = default;
    [[nodiscard]] double mirror_axis_x() const { return target.mid_x(); }
    [[nodiscard]] CompositionPoint destination_point(
        CompositionPoint source_point, bool mirrored_horizontally) const;
};

class CameraContentFill {
public:
    static constexpr double kEdgeOverscanScale = 1.01;
    [[nodiscard]] static std::optional<CameraContentFillLayout> layout(
        double source_width, double source_height, CompositionRect target,
        NormalizedPoint focus = {0.5, 0.5}, double content_scale = 1);
};

enum class ScreenFrameStyle {
    kNone,
    kWindowLight,
    kWindowDark,
    kBrowserLight,
    kBrowserDark,
    kDevicePhone,
    kDeviceTablet,
    kDevicePhonePortrait,
    kDevicePhoneLandscape,
    kDeviceTabletPortrait,
    kDeviceTabletLandscape,
};

[[nodiscard]] ScreenFrameStyle screen_frame_style_from_raw(std::string_view value);

struct ScreenDecorationInsets {
    double top = 0;
    double right = 0;
    double bottom = 0;
    double left = 0;
    constexpr bool operator==(const ScreenDecorationInsets&) const = default;
};

class ScreenFrameGeometry {
public:
    [[nodiscard]] static ScreenDecorationInsets decoration_insets_at_scale_one(
        ScreenFrameStyle style, double frame_scale, double toolbar_scale,
        double fitted_width, double fitted_height, double style_scale);
};

}  // namespace dogsc
