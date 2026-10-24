#include "dogsc/scene_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <array>
#include <limits>
namespace dogsc {
namespace {
constexpr double kProjectionEpsilon = 1e-9;
struct PointMapper {
    CompositionRect source{};
    CompositionPoint top_left{};
    double a = 0;
    double b = 0;
    double d = 0;
    double e = 0;
    double perspective_x = 0;
    double perspective_y = 0;

    static std::optional<PointMapper> make(const ProjectedScreenQuad& quad,
                                           CompositionRect source_rect) {
        if (!std::isfinite(source_rect.width) || !std::isfinite(source_rect.height) ||
            source_rect.width <= 0 || source_rect.height <= 0) return std::nullopt;
        for (const auto& point : quad.corners()) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) return std::nullopt;
        }
        const double dx1 = quad.top_right.x - quad.bottom_right.x;
        const double dx2 = quad.bottom_left.x - quad.bottom_right.x;
        const double dx3 = quad.top_left.x - quad.top_right.x + quad.bottom_right.x - quad.bottom_left.x;
        const double dy1 = quad.top_right.y - quad.bottom_right.y;
        const double dy2 = quad.bottom_left.y - quad.bottom_right.y;
        const double dy3 = quad.top_left.y - quad.top_right.y + quad.bottom_right.y - quad.bottom_left.y;
        const double denominator = dx1 * dy2 - dx2 * dy1;
        PointMapper result;
        result.source = source_rect;
        result.top_left = quad.top_left;
        if (std::abs(dx3) > kProjectionEpsilon || std::abs(dy3) > kProjectionEpsilon) {
            if (std::abs(denominator) <= kProjectionEpsilon) return std::nullopt;
            result.perspective_x = (dx3 * dy2 - dx2 * dy3) / denominator;
            result.perspective_y = (dx1 * dy3 - dx3 * dy1) / denominator;
        }
        result.a = quad.top_right.x - quad.top_left.x + result.perspective_x * quad.top_right.x;
        result.b = quad.bottom_left.x - quad.top_left.x + result.perspective_y * quad.bottom_left.x;
        result.d = quad.top_right.y - quad.top_left.y + result.perspective_x * quad.top_right.y;
        result.e = quad.bottom_left.y - quad.top_left.y + result.perspective_y * quad.bottom_left.y;
        return result;
    }

    std::optional<CompositionPoint> map(CompositionPoint point) const {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return std::nullopt;
        const double x = (point.x - source.x) / source.width;
        const double y = (point.y - source.y) / source.height;
        const double divisor = perspective_x * x + perspective_y * y + 1;
        if (!std::isfinite(divisor) || std::abs(divisor) <= kProjectionEpsilon) return std::nullopt;
        const CompositionPoint mapped{
            (a * x + b * y + top_left.x) / divisor,
            (d * x + e * y + top_left.y) / divisor,
        };
        if (!std::isfinite(mapped.x) || !std::isfinite(mapped.y)) return std::nullopt;
        return mapped;
    }

    std::optional<CompositionPoint> inverse(CompositionPoint point) const {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return std::nullopt;
        const double aa = a - point.x * perspective_x;
        const double bb = b - point.x * perspective_y;
        const double dd = d - point.y * perspective_x;
        const double ee = e - point.y * perspective_y;
        const double determinant = aa * ee - bb * dd;
        if (std::abs(determinant) <= kProjectionEpsilon) return std::nullopt;
        const double x = point.x - top_left.x;
        const double y = point.y - top_left.y;
        const double u = (x * ee - bb * y) / determinant;
        const double v = (aa * y - x * dd) / determinant;
        const CompositionPoint mapped{source.x + u * source.width, source.y + v * source.height};
        if (!std::isfinite(mapped.x) || !std::isfinite(mapped.y)) return std::nullopt;
        return mapped;
    }
};

double cross(CompositionPoint start, CompositionPoint end, CompositionPoint point) {
    return (end.x - start.x) * (point.y - start.y) -
           (end.y - start.y) * (point.x - start.x);
}

bool lies_on_segment(CompositionPoint point, CompositionPoint start,
                     CompositionPoint end, double tolerance) {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double length_squared = dx * dx + dy * dy;
    if (length_squared <= tolerance * tolerance) {
        return std::hypot(point.x - start.x, point.y - start.y) <= tolerance;
    }
    const double parameter = ((point.x - start.x) * dx + (point.y - start.y) * dy) / length_squared;
    if (parameter < -tolerance || parameter > 1 + tolerance) return false;
    const CompositionPoint nearest{start.x + parameter * dx, start.y + parameter * dy};
    return std::hypot(point.x - nearest.x, point.y - nearest.y) <= tolerance;
}
}  // namespace

CompositionRect ProjectedScreenQuad::bounds() const {
    const auto points = corners();
    double min_x = points[0].x, max_x = points[0].x;
    double min_y = points[0].y, max_y = points[0].y;
    for (const auto& point : points) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    return {min_x, min_y, std::max(max_x - min_x, 0.0), std::max(max_y - min_y, 0.0)};
}

std::array<CompositionPoint, 4> ProjectedScreenQuad::corners() const {
    return {top_left, top_right, bottom_right, bottom_left};
}

CompositionPoint ProjectedScreenQuad::center() const {
    const auto points = corners();
    CompositionPoint result{};
    for (const auto& point : points) { result.x += point.x / 4; result.y += point.y / 4; }
    return result;
}

std::optional<CompositionPoint> ProjectedScreenQuad::project(CompositionPoint point,
                                                              CompositionRect source_rect) const {
    const auto mapper = PointMapper::make(*this, source_rect);
    return mapper ? mapper->map(point) : std::nullopt;
}

std::optional<CompositionPoint> ProjectedScreenQuad::unproject(CompositionPoint point,
                                                                CompositionRect source_rect) const {
    const auto mapper = PointMapper::make(*this, source_rect);
    return mapper ? mapper->inverse(point) : std::nullopt;
}

double ProjectedScreenQuad::semantic_resize_delta(CompositionPoint translation,
                                                   double reference_length) const {
    if (!std::isfinite(translation.x) || !std::isfinite(translation.y) ||
        !std::isfinite(reference_length)) return 0;
    const double dx = bottom_right.x - top_left.x;
    const double dy = bottom_right.y - top_left.y;
    const double length = std::hypot(dx, dy);
    if (length <= 1e-6) return 0;
    const double unit_x = dx / length;
    const double unit_y = dy / length;
    const double travel = translation.x * unit_x + translation.y * unit_y;
    return travel * (std::abs(unit_x) + std::abs(unit_y)) / std::max(reference_length, 1.0);
}

bool ProjectedScreenQuad::contains(CompositionPoint point, double tolerance) const {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(tolerance)) return false;
    const auto points = corners();
    if (std::any_of(points.begin(), points.end(), [](auto value) {
        return !std::isfinite(value.x) || !std::isfinite(value.y);
    })) return false;
    const double epsilon = std::max(tolerance, 0.0);
    double twice_area = 0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto start = points[index];
        const auto end = points[(index + 1) % points.size()];
        twice_area += start.x * end.y - end.x * start.y;
    }
    if (std::abs(twice_area) <= epsilon) return false;
    int winding = 0;
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto start = points[index];
        const auto end = points[(index + 1) % points.size()];
        if (lies_on_segment(point, start, end, epsilon)) return true;
        if (start.y <= point.y) {
            if (end.y > point.y && cross(start, end, point) > epsilon) ++winding;
        } else if (end.y <= point.y && cross(start, end, point) < -epsilon) {
            --winding;
        }
    }
    return winding != 0;
}

ProjectedScreenQuad ScreenProjection::project(CompositionRect rect,
                                               double rotation_x, double rotation_y,
                                               double rotation_z, double perspective,
                                               std::optional<CompositionPoint> anchor) {
    const double safe_width = std::max(rect.width, 0.0);
    const double safe_height = std::max(rect.height, 0.0);
    const auto pivot = anchor && std::isfinite(anchor->x) && std::isfinite(anchor->y)
        ? *anchor : CompositionPoint{rect.mid_x(), rect.mid_y()};
    const double maximum_dimension = std::max({rect.width, rect.height, 1.0});
    const double depth = std::clamp(perspective, 0.0, 2.0);
    const double camera_distance = depth > 0 ? maximum_dimension * 2 / std::max(depth, 1e-4)
                                             : std::numeric_limits<double>::infinity();
    const double rx = rotation_x * std::numbers::pi / 180;
    const double ry = rotation_y * std::numbers::pi / 180;
    const double rz = rotation_z * std::numbers::pi / 180;
    const double sin_x = std::sin(rx), cos_x = std::cos(rx);
    const double sin_y = std::sin(ry), cos_y = std::cos(ry);
    const double sin_z = std::sin(rz), cos_z = std::cos(rz);
    const auto point = [&](double x, double y) {
        const double x_after_x = x;
        const double y_after_x = y * cos_x;
        const double z_after_x = y * sin_x;
        const double x_after_y = x_after_x * cos_y + z_after_x * sin_y;
        const double y_after_y = y_after_x;
        const double z_after_y = -x_after_x * sin_y + z_after_x * cos_y;
        const double x_after_z = x_after_y * cos_z - y_after_y * sin_z;
        const double y_after_z = x_after_y * sin_z + y_after_y * cos_z;
        const double denominator = std::isfinite(camera_distance)
            ? std::max(camera_distance - z_after_y, camera_distance * 0.12) : 1.0;
        const double projection_scale = std::isfinite(camera_distance) ? camera_distance / denominator : 1.0;
        return CompositionPoint{pivot.x + x_after_z * projection_scale,
                                pivot.y + y_after_z * projection_scale};
    };
    return {
        point(rect.x - pivot.x, rect.y - pivot.y),
        point(rect.x + safe_width - pivot.x, rect.y - pivot.y),
        point(rect.x + safe_width - pivot.x, rect.y + safe_height - pivot.y),
        point(rect.x - pivot.x, rect.y + safe_height - pivot.y),
    };
}

CompositionPoint CameraContentFillLayout::destination_point(CompositionPoint point,
                                                             bool mirrored) const {
    const double x = mirrored ? source.x + source.width - (point.x - source.x) : point.x;
    return {destination.x + (x - source.x) * scale,
            destination.y + (point.y - source.y) * scale};
}

std::optional<CameraContentFillLayout> CameraContentFill::layout(
    double source_width, double source_height, CompositionRect target,
    NormalizedPoint focus, double content_scale) {
    if (!std::isfinite(source_width) || !std::isfinite(source_height) ||
        !std::isfinite(target.x) || !std::isfinite(target.y) ||
        !std::isfinite(target.width) || !std::isfinite(target.height) ||
        source_width <= 0 || source_height <= 0 || target.width <= 0 || target.height <= 0) return std::nullopt;
    const double source_zoom = std::clamp(std::isfinite(content_scale) ? content_scale : 1.0, 1.0, 4.0);
    const double scale = std::max(target.width / source_width, target.height / source_height) *
                         kEdgeOverscanScale * source_zoom;
    const double destination_width = source_width * scale;
    const double destination_height = source_height * scale;
    const double visible_width = target.width / scale;
    const double visible_height = target.height / scale;
    const double focus_x = std::clamp(std::isfinite(focus.x) ? focus.x : 0.5, 0.0, 1.0);
    const double focus_y = std::clamp(std::isfinite(focus.y) ? focus.y : 0.5, 0.0, 1.0);
    const CompositionRect visible{(source_width - visible_width) * focus_x,
                                  (source_height - visible_height) * focus_y,
                                  visible_width, visible_height};
    const CompositionRect destination{target.x - visible.x * scale,
                                      target.y - visible.y * scale,
                                      destination_width, destination_height};
    return CameraContentFillLayout{{0, 0, source_width, source_height}, target,
                                   destination, visible, scale};
}
ScreenFrameStyle screen_frame_style_from_raw(std::string_view value) {
    if (value == "windowLight") return ScreenFrameStyle::kWindowLight;
    if (value == "windowDark") return ScreenFrameStyle::kWindowDark;
    if (value == "browserLight") return ScreenFrameStyle::kBrowserLight;
    if (value == "browserDark") return ScreenFrameStyle::kBrowserDark;
    if (value == "devicePhone") return ScreenFrameStyle::kDevicePhone;
    if (value == "deviceTablet") return ScreenFrameStyle::kDeviceTablet;
    if (value == "devicePhonePortrait") return ScreenFrameStyle::kDevicePhonePortrait;
    if (value == "devicePhoneLandscape") return ScreenFrameStyle::kDevicePhoneLandscape;
    if (value == "deviceTabletPortrait") return ScreenFrameStyle::kDeviceTabletPortrait;
    if (value == "deviceTabletLandscape") return ScreenFrameStyle::kDeviceTabletLandscape;
    return ScreenFrameStyle::kNone;
}

ScreenDecorationInsets ScreenFrameGeometry::decoration_insets_at_scale_one(
    ScreenFrameStyle style, double frame_scale, double toolbar_scale,
    double fitted_width, double fitted_height, double style_scale) {
    if (style == ScreenFrameStyle::kNone) return {};
    const double safe_style_scale = std::max(style_scale, 0.0001);
    const double authored_frame_scale = std::clamp(frame_scale, 0.6, 1.6);
    const double authored_toolbar_scale = std::clamp(toolbar_scale, 0.65, 1.6);
    const double visual_scale = safe_style_scale * authored_frame_scale;
    switch (style) {
        case ScreenFrameStyle::kNone:
            return {};
        case ScreenFrameStyle::kWindowLight:
        case ScreenFrameStyle::kWindowDark:
        case ScreenFrameStyle::kBrowserLight:
        case ScreenFrameStyle::kBrowserDark: {
            const double ideal_height = 46 * visual_scale * authored_toolbar_scale;
            const double minimum = 4 * safe_style_scale;
            return {std::min(std::max(ideal_height, minimum),
                             std::max(std::max(fitted_height, 0.0) * 0.25, minimum)), 0, 0, 0};
        }
        case ScreenFrameStyle::kDevicePhone:
        case ScreenFrameStyle::kDevicePhonePortrait:
        case ScreenFrameStyle::kDevicePhoneLandscape: {
            const double short_edge = std::max(std::min(fitted_width, fitted_height), 1.0);
            const double bezel = std::min(std::max(short_edge * 0.026 * authored_frame_scale,
                                                   2 * safe_style_scale), short_edge * 0.07);
            return {bezel, bezel, bezel, bezel};
        }
        case ScreenFrameStyle::kDeviceTablet:
        case ScreenFrameStyle::kDeviceTabletPortrait:
        case ScreenFrameStyle::kDeviceTabletLandscape: {
            const double short_edge = std::max(std::min(fitted_width, fitted_height), 1.0);
            const double bezel = std::min(std::max(short_edge * 0.035 * authored_frame_scale,
                                                   2 * safe_style_scale), short_edge * 0.08);
            return {bezel, bezel, bezel, bezel};
        }
    }
    return {};
}
}  // namespace dogsc
