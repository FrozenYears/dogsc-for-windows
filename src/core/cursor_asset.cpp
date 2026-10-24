#include "dogsc/cursor_asset.hpp"
#include "dogsc/project_model.hpp"

#include <algorithm>
#include <numbers>
#include <cmath>

namespace dogsc {

std::vector<CursorAssetID> CursorAssetID::system_catalog() {
    return {
        system_arrow(),      system_pointing_hand(), system_crosshair(),
        system_ibeam(),      system_open_hand(),     system_closed_hand(),
        system_not_allowed(), touch_dot(),           hidden(),
    };
}

bool CursorRenderGeometry::layout(double point_x, double point_y, double canvas_short_edge,
                                  double style_size, const CursorAssetMetrics& metrics,
                                  CursorRenderLayout& out) {
    const double reference_height = metrics.scale_reference_height.value_or(metrics.height);
    if (!std::isfinite(point_x) || !std::isfinite(point_y) ||
        !std::isfinite(canvas_short_edge) || canvas_short_edge <= 0 ||
        !std::isfinite(metrics.width) || !std::isfinite(metrics.height) ||
        !std::isfinite(reference_height) || metrics.width <= 0 || metrics.height <= 0 ||
        reference_height <= 0) {
        return false;
    }

    const double multiplier = std::clamp(std::isfinite(style_size) ? style_size : 1.0, 0.25, 6.0);
    const double reference = std::max(
        canvas_short_edge * CursorRenderGeometry::kCanonicalHeight /
            CursorRenderGeometry::kCanonicalShortEdge * multiplier,
        1.0);
    const double image_scale = reference / reference_height;
    const double width = metrics.width * image_scale;
    const double height = metrics.height * image_scale;
    if (!(width > 0) || !(height > 0)) return false;

    const double hotspot_x = std::clamp(metrics.hotspot_x, 0.0, 1.0) * width;
    const double hotspot_y = std::clamp(metrics.hotspot_y, 0.0, 1.0) * height;
    out.origin_x = point_x - hotspot_x;
    out.origin_y = point_y - hotspot_y;
    out.width = width;
    out.height = height;
    out.hotspot_x = hotspot_x;
    out.hotspot_y = hotspot_y;
    out.click_effect_width = reference * 1.45;
    out.click_effect_height = reference;
    out.click_line_width = std::max(reference * 0.08,
                                    canvas_short_edge * 2 / CursorRenderGeometry::kCanonicalShortEdge);
    return true;
}

std::optional<CursorClickGeometryFrame> CursorRenderGeometry::click_geometry(
    CursorClickEffectStyle style, double progress, double base_height,
    double opacity_multiplier, double scale_multiplier) {
    if (style == CursorClickEffectStyle::kNone || !std::isfinite(progress) ||
        progress < 0 || progress > 1 || !std::isfinite(base_height) || base_height <= 0) {
        return std::nullopt;
    }
    const double user_scale = std::clamp(std::isfinite(scale_multiplier) ? scale_multiplier : 1.0,
                                         0.4, 3.0);
    const double user_opacity = std::clamp(std::isfinite(opacity_multiplier) ? opacity_multiplier : 1.0,
                                           0.05, 1.0);
    const double height = base_height * user_scale;
    CursorClickGeometryFrame frame;
    frame.style = style;
    frame.progress = progress;
    switch (style) {
        case CursorClickEffectStyle::kRipple: {
            const double eased = 1 - std::pow(1 - progress, 3);
            frame.primary_diameter = height * (0.45 + 1.45 * eased);
            frame.primary_line_width = std::max(height * (0.075 - 0.04 * progress), 1.2);
            frame.primary_opacity = std::max(user_opacity * 0.95 * (1 - std::pow(progress, 1.3)), 0.0);
            frame.primary_glow_radius = height * 0.10 * (1 - progress);
            if (progress < 0.35) {
                const double dot = progress / 0.35;
                frame.secondary_diameter = height * 0.28 * (1 - dot);
                frame.secondary_opacity = std::max(user_opacity * 0.85 * (1 - dot), 0.0);
                frame.secondary_is_filled = true;
                frame.secondary_glow_radius = height * 0.08 * (1 - dot);
            }
            frame.max_footprint_diameter = height * 2.2;
            break;
        }
        case CursorClickEffectStyle::kGlow: {
            if (progress < 0.18) {
                const double bloom = progress / 0.18;
                frame.primary_diameter = height * (0.75 + 0.50 * (1 - std::pow(1 - bloom, 2)));
                frame.primary_opacity = std::max(user_opacity * 0.78 * bloom, 0.0);
            } else {
                const double fade = (progress - 0.18) / 0.82;
                frame.primary_diameter = height * (1.25 + 0.55 * fade);
                frame.primary_opacity = std::max(user_opacity * 0.78 * (1 - std::pow(fade, 1.4)), 0.0);
            }
            frame.primary_is_filled = true;
            frame.primary_glow_radius = height * 0.30;
            frame.secondary_diameter = frame.primary_diameter * 0.50;
            frame.secondary_opacity = frame.primary_opacity * 0.75;
            frame.secondary_is_filled = true;
            frame.secondary_glow_radius = height * 0.18;
            frame.max_footprint_diameter = height * 2.2;
            break;
        }
        case CursorClickEffectStyle::kPulse: {
            const double eased = 1 - std::pow(1 - progress, 2.5);
            frame.primary_diameter = height * (0.50 + 1.25 * eased);
            frame.primary_line_width = std::max(height * 0.065, 1.2);
            frame.primary_opacity = std::max(user_opacity * 0.90 * (1 - progress), 0.0);
            frame.primary_glow_radius = height * 0.10 * (1 - progress);
            if (progress <= 0.55) {
                const double inner = progress / 0.55;
                frame.secondary_diameter = height * (0.25 + 0.45 * std::sin(inner * std::numbers::pi));
                frame.secondary_opacity = std::max(user_opacity * 0.55 * (1 - inner), 0.0);
                frame.secondary_is_filled = true;
                frame.secondary_glow_radius = height * 0.12 * (1 - inner);
            }
            frame.max_footprint_diameter = height * 2.0;
            break;
        }
        case CursorClickEffectStyle::kBurst: {
            frame.primary_diameter = std::max(height * (0.35 - 0.18 * progress), 1.0);
            frame.primary_opacity = std::max(user_opacity * 0.95 * (1 - std::pow(progress, 2)), 0.0);
            frame.primary_is_filled = true;
            frame.primary_glow_radius = height * 0.14 * (1 - progress);
            frame.secondary_diameter = height * (0.45 + 0.55 * progress);
            frame.secondary_line_width = std::max(height * 0.035, 1.0);
            frame.secondary_opacity = std::max(user_opacity * 0.55 * (1 - progress), 0.0);
            frame.secondary_glow_radius = height * 0.08 * (1 - progress);
            const double eased = 1 - std::pow(1 - progress, 2);
            frame.accent_offset = height * (0.28 + 0.38 * eased);
            frame.accent_size = std::max(height * 0.075 * (1 - progress), 1.2);
            frame.accent_opacity = std::max(user_opacity * 0.85 * (1 - progress), 0.0);
            frame.max_footprint_diameter = height * 1.8;
            break;
        }
        case CursorClickEffectStyle::kNone:
            return std::nullopt;
    }
    return frame;
}

}  // namespace dogsc
