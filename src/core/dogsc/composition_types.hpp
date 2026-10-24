#pragma once

#include <algorithm>

namespace dogsc {

/// 画布几何采用左上原点，不依赖平台图形 SDK。
struct CompositionSize {
    double width = 0;
    double height = 0;
    constexpr bool operator==(const CompositionSize&) const = default;
    [[nodiscard]] double short_edge() const { return std::min(width, height); }
};
struct CompositionPoint {
    double x = 0;
    double y = 0;
    constexpr bool operator==(const CompositionPoint&) const = default;
};
struct CompositionRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    constexpr bool operator==(const CompositionRect&) const = default;
    [[nodiscard]] double mid_x() const { return x + width / 2; }
    [[nodiscard]] double mid_y() const { return y + height / 2; }
};

}  // namespace dogsc
