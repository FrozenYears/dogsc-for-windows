#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "dogsc/core_types.hpp"

namespace dogsc {

/// 归一化点坐标：(0,0) 为左上角，(1,1) 为右下角。
struct NormalizedPoint {
    double x = 0;
    double y = 0;

    constexpr bool operator==(const NormalizedPoint&) const = default;
};

/// 归一化裁剪矩形，原点在源图左上角。
struct NormalizedCrop {
    double x = 0;
    double y = 0;
    double width = 1;
    double height = 1;

    constexpr bool operator==(const NormalizedCrop&) const = default;

    static constexpr NormalizedCrop full() { return {}; }

    [[nodiscard]] NormalizedCrop clamped(double minimum_size = 0.02) const;
};

/// 归一化覆盖层矩形，原点在画布左上角。
struct NormalizedOverlayRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;

    constexpr bool operator==(const NormalizedOverlayRect&) const = default;

    static constexpr NormalizedOverlayRect centered() { return {0.35, 0.39, 0.30, 0.22}; }

    [[nodiscard]] NormalizedOverlayRect clamped() const;
};

/// 颜色以 `#RRGGBB` 或 `#RRGGBBAA` 存储，与上游 `HexColor` 的持久化形态一致。
struct HexColor {
    std::string value = "#FFFFFF";

    constexpr bool operator==(const HexColor&) const = default;

    static HexColor white() { return {"#FFFFFF"}; }
    static HexColor black() { return {"#000000"}; }
};

/// 媒体时间区间，`duration` 恒为非负。
struct MediaTimeRange {
    TimeInterval start = 0;
    TimeInterval duration = 0;

    constexpr bool operator==(const MediaTimeRange&) const = default;

    [[nodiscard]] TimeInterval end() const { return start + duration; }
};

}  // namespace dogsc
