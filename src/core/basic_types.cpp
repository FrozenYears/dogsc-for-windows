#include "dogsc/basic_types.hpp"

#include <algorithm>

namespace dogsc {

NormalizedCrop NormalizedCrop::clamped(double minimum_size) const {
    const double minimum = std::clamp(minimum_size, 0.001, 1.0);
    const double safe_x = std::clamp(x, 0.0, 1.0 - minimum);
    const double safe_y = std::clamp(y, 0.0, 1.0 - minimum);
    return NormalizedCrop{
        .x = safe_x,
        .y = safe_y,
        .width = std::clamp(width, minimum, 1.0 - safe_x),
        .height = std::clamp(height, minimum, 1.0 - safe_y),
    };
}

NormalizedOverlayRect NormalizedOverlayRect::clamped() const {
    const double safe_width = std::clamp(std::isfinite(width) ? width : 0.30, 0.01, 1.0);
    const double safe_height = std::clamp(std::isfinite(height) ? height : 0.22, 0.01, 1.0);
    return NormalizedOverlayRect{
        .x = std::clamp(std::isfinite(x) ? x : 0.35, 0.0, 1.0 - safe_width),
        .y = std::clamp(std::isfinite(y) ? y : 0.39, 0.0, 1.0 - safe_height),
        .width = safe_width,
        .height = safe_height,
    };
}

}  // namespace dogsc
