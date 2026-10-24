#pragma once

#include <optional>
#include <string>
#include <vector>
#include "dogsc/basic_types.hpp"

namespace dogsc {
enum class CursorClickEffectStyle;

/// `project.json` 中持久化的光标资源标识。
///
/// 存储值保持语言中立，显示名与美术资源由 UI 层编目，
/// 这样标签与图形可以演进而不会让已有工程失效。
/// 本产品不与 macOS 端互通，`rawValue` 无需逐值对齐，
/// 但四类语义值必须存在以保证编辑语义完整。
struct CursorAssetID {
    std::string raw_value;

    constexpr bool operator==(const CursorAssetID&) const = default;

    [[nodiscard]] bool empty() const { return raw_value.empty(); }

    static CursorAssetID system_arrow() { return {"system.arrow"}; }
    /// 表示「沿录制时实际系统光标走」；渲染前必须解析成具体资源。
    static CursorAssetID automatic() { return {"system.automatic"}; }
    static CursorAssetID system_pointing_hand() { return {"system.pointing-hand"}; }
    static CursorAssetID system_crosshair() { return {"system.crosshair"}; }
    static CursorAssetID system_ibeam() { return {"system.ibeam"}; }
    static CursorAssetID system_open_hand() { return {"system.open-hand"}; }
    static CursorAssetID system_closed_hand() { return {"system.closed-hand"}; }
    static CursorAssetID system_not_allowed() { return {"system.not-allowed"}; }
    static CursorAssetID touch_dot() { return {"system.touch-dot"}; }
    static CursorAssetID hidden() { return {"system.hidden"}; }

    /// 所有仍可能出现在工程文件里的系统资源，用于资源完整性校验。
    static std::vector<CursorAssetID> system_catalog();
};

/// 光标图形度量。热点为图形内的归一化坐标。
struct CursorAssetMetrics {
    std::string asset_id;
    double width = 1;
    double height = 1;
    double hotspot_x = 0;
    double hotspot_y = 0;
    double shadow_radius = 0;
    HexColor click_color = HexColor::black();
    std::optional<double> scale_reference_height;
    constexpr bool operator==(const CursorAssetMetrics&) const = default;
};

/// 光标在画布上的 2D 布局结果。
///
/// 位置按热点锚定，尺寸以画布短边为基准缩放，
/// 保证 1080p 与 4K 导出的视觉比例一致。
struct CursorRenderLayout {
    double origin_x = 0;
    double origin_y = 0;
    double width = 0;
    double height = 0;
    double hotspot_x = 0;
    double hotspot_y = 0;
    double click_effect_width = 0;
    double click_effect_height = 0;
    double click_line_width = 0;
    constexpr bool operator==(const CursorRenderLayout&) const = default;
};

struct CursorClickGeometryFrame {
    CursorClickEffectStyle style{};
    double progress = 0;
    double primary_diameter = 0;
    double primary_line_width = 0;
    double primary_opacity = 0;
    bool primary_is_filled = false;
    double primary_glow_radius = 0;
    double secondary_diameter = 0;
    double secondary_line_width = 0;
    double secondary_opacity = 0;
    bool secondary_is_filled = false;
    double secondary_glow_radius = 0;
    double accent_offset = 0;
    double accent_size = 0;
    double accent_opacity = 0;
    double max_footprint_diameter = 0;
    constexpr bool operator==(const CursorClickGeometryFrame&) const = default;
};

/// 纯几何布局，不依赖任何渲染后端。
class CursorRenderGeometry {
public:
    static constexpr double kCanonicalHeight = 44;
    static constexpr double kCanonicalShortEdge = 1080;
    /// `point` 为热点在画布上的像素位置；输出尺度以 1080 短边 44px 为基准。
    static bool layout(double point_x, double point_y, double canvas_short_edge,
                       double style_size, const CursorAssetMetrics& metrics,
                       CursorRenderLayout& out);
    [[nodiscard]] static std::optional<CursorClickGeometryFrame> click_geometry(
        CursorClickEffectStyle style, double progress, double base_height,
        double opacity_multiplier = 1.0, double scale_multiplier = 1.0);
};

}  // namespace dogsc
