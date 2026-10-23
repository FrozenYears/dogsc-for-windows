#include "dogsc/project_timeline.hpp"

#include <algorithm>
#include <cmath>

namespace dogsc {

ZoomBezierCurve ZoomBezierCurve::linear() { return {0, 0, 1, 1}; }
ZoomBezierCurve ZoomBezierCurve::quadratic() { return {0.45, 0, 0.55, 1}; }
ZoomBezierCurve ZoomBezierCurve::cubic() { return {0.65, 0, 0.35, 1}; }
ZoomBezierCurve ZoomBezierCurve::quintic() { return {0.82, 0, 0.18, 1}; }

ZoomBezierCurve ZoomBezierCurve::for_preset(ZoomEasingPreset preset) {
    switch (preset) {
        case ZoomEasingPreset::kLinear:
            return linear();
        case ZoomEasingPreset::kQuadratic:
            return quadratic();
        case ZoomEasingPreset::kQuintic:
            return quintic();
        case ZoomEasingPreset::kCubic:
        case ZoomEasingPreset::kSpring:
        case ZoomEasingPreset::kCustom:
            return cubic();
    }
    return cubic();
}

NormalizedPoint ZoomBezierCurve::point_at(double parameter) const {
    const double t = std::clamp(parameter, 0.0, 1.0);
    const double inverse = 1 - t;
    const double x =
        3 * inverse * inverse * t * x1 + 3 * inverse * t * t * x2 + t * t * t;
    const double y =
        3 * inverse * inverse * t * y1 + 3 * inverse * t * t * y2 + t * t * t;
    return {x, y};
}

double ZoomBezierCurve::value_at(double normalized_time) const {
    const double target_x = std::clamp(normalized_time, 0.0, 1.0);
    if (target_x <= 0 || target_x >= 1) return target_x;
    double parameter = target_x;

    for (int iteration = 0; iteration < 7; ++iteration) {
        const NormalizedPoint point = point_at(parameter);
        const double error = point.x - target_x;
        if (std::fabs(error) < 0.00001) return point.y;
        const double t = std::clamp(parameter, 0.0, 1.0);
        const double inverse = 1 - t;
        const double derivative = 3 * inverse * inverse * x1 +
                                  6 * inverse * t * (x2 - x1) + 3 * t * t * (1 - x2);
        if (std::fabs(derivative) < 0.00001) break;
        const double next = parameter - error / derivative;
        if (next < 0 || next > 1) break;
        parameter = next;
    }

    double lower = 0;
    double upper = 1;
    parameter = target_x;
    for (int iteration = 0; iteration < 18; ++iteration) {
        const NormalizedPoint point = point_at(parameter);
        if (std::fabs(point.x - target_x) < 0.00001) return point.y;
        if (point.x < target_x) {
            lower = parameter;
        } else {
            upper = parameter;
        }
        parameter = (lower + upper) / 2;
    }
    return point_at(parameter).y;
}

TimeInterval TransitionTiming::lead_in_end_time() const {
    return start_time + std::min(lead_in_duration, duration);
}

TimeInterval TransitionTiming::requested_return_duration(TimeInterval default_transition) const {
    if (preferred_return_duration) return *preferred_return_duration;
    return return_duration > 0.0001 ? return_duration : std::max(default_transition, 0.0);
}

void TransitionTiming::preserve_transition_intent(TimeInterval default_transition) {
    if (!preferred_lead_in_duration) preferred_lead_in_duration = lead_in_duration;
    if (!preferred_return_duration) {
        preferred_return_duration = requested_return_duration(default_transition);
    }
}

double element_motion_progress(double linear_progress, ElementMotionCurve curve) {
    const double x = std::clamp(std::isfinite(linear_progress) ? linear_progress : 0.0, 0.0, 1.0);
    switch (curve) {
        case ElementMotionCurve::kSmooth:
            return x * x * x * (x * (x * 6 - 15) + 10);
        case ElementMotionCurve::kSwift:
            return 1 - std::pow(1 - x, 4);
        case ElementMotionCurve::kGentle:
            return 1 - std::pow(1 - x, 3);
    }
    return x;
}

}  // namespace dogsc
