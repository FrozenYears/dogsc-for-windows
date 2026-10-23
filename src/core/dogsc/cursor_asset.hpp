#pragma once

#include <string>
#include <vector>

namespace dogsc {

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
};

/// 纯几何布局，不依赖任何渲染后端。
class CursorRenderGeometry {
public:
    /// `point` 为热点在画布上的像素位置；`canvasShortEdge` 取画布短边。
    /// 度量缺失或参数非有限时返回空，调用方应跳过光标图层。
    static bool layout(double point_x, double point_y, double canvas_short_edge,
                       double style_size, const CursorAssetMetrics& metrics,
                       CursorRenderLayout& out);
};

}  // namespace dogsc
