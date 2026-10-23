#include "dogsc/cursor_asset.hpp"

#include <algorithm>
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
    if (!std::isfinite(point_x) || !std::isfinite(point_y) ||
        !std::isfinite(canvas_short_edge) || !std::isfinite(style_size) ||
        canvas_short_edge <= 0 || !std::isfinite(metrics.width) ||
        !std::isfinite(metrics.height) || metrics.width <= 0 || metrics.height <= 0) {
        return false;
    }

    // 上游以 44px 的画布短边基准定义光标视觉尺寸；
    // 乘上样式倍率后按画布短边等比放大。
    const double safe_style_size = std::clamp(style_size, 0.25, 6.0);
    const double unit = canvas_short_edge / 44.0 * safe_style_size;
    const double width = metrics.width * unit;
    const double height = metrics.height * unit;
    if (!(width > 0) || !(height > 0)) return false;

    const double hotspot_x = std::clamp(metrics.hotspot_x, 0.0, 1.0) * width;
    const double hotspot_y = std::clamp(metrics.hotspot_y, 0.0, 1.0) * height;

    out.origin_x = point_x - hotspot_x;
    out.origin_y = point_y - hotspot_y;
    out.width = width;
    out.height = height;
    out.hotspot_x = hotspot_x;
    out.hotspot_y = hotspot_y;
    // 点击特效按光标高度归一，保证不同缩放下波纹与光标同比例。
    out.click_effect_width = height;
    out.click_effect_height = height;
    return true;
}

}  // namespace dogsc
