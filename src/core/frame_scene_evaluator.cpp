#include "dogsc/frame_scene_evaluator.hpp"

#include <algorithm>
#include <cmath>

namespace dogsc {
namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

TimeInterval OpeningSequence::maximum_stagger(size_t element_count) const {
    const size_t gap_count = element_count > 0 ? element_count - 1 : 0;
    // 零个或一个参与者时没有需要约束的间隔；
    // 保留用户作者值，避免重新启用其它元素时静默重置节奏。
    if (gap_count == 0) return 1.2;
    return std::min(1.2, std::max(duration - kMinimumElementDuration, 0.0) /
                            static_cast<double>(gap_count));
}

std::vector<OpeningSequenceElement> OpeningSequence::available_elements(
    bool has_camera, bool has_opening_sticker) const {
    std::vector<OpeningSequenceElement> result;
    for (OpeningSequenceElement element : element_order) {
        switch (element) {
            case OpeningSequenceElement::kScreen:
                result.push_back(element);
                break;
            case OpeningSequenceElement::kCamera:
                if (has_camera) result.push_back(element);
                break;
            case OpeningSequenceElement::kStickers:
                if (has_opening_sticker) result.push_back(element);
                break;
        }
    }
    return result;
}

std::vector<OpeningSequenceElement> OpeningSequence::participating_elements(
    bool has_camera, bool has_opening_sticker) const {
    std::vector<OpeningSequenceElement> result;
    for (OpeningSequenceElement element : available_elements(has_camera, has_opening_sticker)) {
        if (std::find(included_elements.begin(), included_elements.end(), element) !=
            included_elements.end()) {
            result.push_back(element);
        }
    }
    return result;
}

double FrameSceneEvaluator::smoother_step(double value) {
    const double x = std::clamp(value, 0.0, 1.0);
    return x * x * x * (x * (x * 6 - 15) + 10);
}

OpeningSample FrameSceneEvaluator::opening_sample(
    const OpeningSequence& sequence, OpeningSequenceElement element,
    const std::vector<OpeningSequenceElement>& active_elements, TimeInterval time,
    double canvas_width, double canvas_height) {
    const auto position =
        std::find(active_elements.begin(), active_elements.end(), element);
    if (!sequence.is_enabled || time >= sequence.duration ||
        position == active_elements.end()) {
        return OpeningSample{};
    }
    const size_t index = static_cast<size_t>(position - active_elements.begin());
    const TimeInterval effective_stagger =
        std::min(sequence.stagger, sequence.maximum_stagger(active_elements.size()));
    const TimeInterval last_delay =
        effective_stagger * static_cast<double>(active_elements.size() > 0 ? active_elements.size() - 1 : 0);
    const TimeInterval item_duration =
        std::max(sequence.duration - last_delay, OpeningSequence::kMinimumElementDuration);
    const TimeInterval start = effective_stagger * static_cast<double>(index);
    const double linear = std::clamp((time - start) / item_duration, 0.0, 1.0);
    const double eased = element_motion_progress(linear, sequence.motion_curve);
    const double opacity = element_motion_progress(std::min(linear * 1.65, 1.0), sequence.motion_curve);
    const double remaining = 1 - eased;
    // 位移要足够快以显得跟手，而 3D 姿态需要在进入画布后仍然可见；
    // 原先共享的 quartic 衰减会在第一个可用画布帧之前就把倾斜抹平。
    const double rotation_remaining = std::pow(1 - linear, 1.5);
    const double overshoot = 1 + 1.35 * std::pow(linear - 1, 3) + 0.35 * std::pow(linear - 1, 2);

    double scale_start = 1;
    CompositionPoint initial_offset{};
    double rotation_x = 0;
    double rotation_y = 0;
    double perspective = 0;

    switch (sequence.preset) {
        case OpeningSequencePreset::kConverge:
            switch (element) {
                case OpeningSequenceElement::kScreen:
                    scale_start = 0.72;
                    initial_offset = {-canvas_width * 1.05, -canvas_height * 0.62};
                    break;
                case OpeningSequenceElement::kCamera:
                    scale_start = 0.58;
                    initial_offset = {canvas_width * 1.12, canvas_height * 0.72};
                    break;
                case OpeningSequenceElement::kStickers:
                    scale_start = 0.62;
                    initial_offset = {-canvas_width * 1.30, -canvas_height * 0.70};
                    break;
            }
            break;
        case OpeningSequencePreset::kSideSlide:
            switch (element) {
                case OpeningSequenceElement::kScreen:
                    scale_start = 0.94;
                    initial_offset = {-canvas_width * 1.12, 0};
                    break;
                case OpeningSequenceElement::kCamera:
                    scale_start = 0.88;
                    initial_offset = {canvas_width * 1.15, 0};
                    break;
                case OpeningSequenceElement::kStickers:
                    scale_start = 0.9;
                    initial_offset = {-canvas_width * 1.35, 0};
                    break;
            }
            break;
        case OpeningSequencePreset::kLight3D:
            switch (element) {
                case OpeningSequenceElement::kScreen:
                    scale_start = 0.80;
                    initial_offset = {-canvas_width * 1.05, canvas_height * 0.08};
                    rotation_x = 9 * rotation_remaining;
                    rotation_y = -22 * rotation_remaining;
                    perspective = 0.72 * rotation_remaining;
                    break;
                case OpeningSequenceElement::kCamera:
                    scale_start = 0.72;
                    initial_offset = {canvas_width * 1.10, canvas_height * 0.16};
                    break;
                case OpeningSequenceElement::kStickers:
                    scale_start = 0.76;
                    initial_offset = {-canvas_width * 1.30, -canvas_height * 0.12};
                    break;
            }
            break;
    }

    const double settled_scale = std::clamp(overshoot, 0.0, 1.04);
    OpeningSample sample;
    sample.opacity = opacity;
    sample.scale = scale_start + (settled_scale - scale_start) * eased;
    sample.offset = {initial_offset.x * remaining, initial_offset.y * remaining};
    sample.rotation_x = rotation_x;
    sample.rotation_y = rotation_y;
    sample.perspective = perspective;
    sample.remaining = remaining;
    return sample;
}

CompositionRect FrameSceneEvaluator::opening_rect(const CompositionRect& rect,
                                                  const OpeningSample& sample) {
    const double width = rect.width * sample.scale;
    const double height = rect.height * sample.scale;
    return CompositionRect{
        .x = rect.mid_x() - width / 2 + sample.offset.x,
        .y = rect.mid_y() - height / 2 + sample.offset.y,
        .width = width,
        .height = height,
    };
}

double FrameSceneEvaluator::overlay_effect_progress(const OverlayTiming& timing,
                                                    int transition_style,
                                                    TimeInterval enter_duration,
                                                    TimeInterval exit_duration,
                                                    TimeInterval time) {
    if (!(timing.duration > 0)) return 0;
    // 0 = none，1 = linear，2 = smooth，与 MosaicTransitionStyle 一致。
    if (transition_style == 0) return 1;
    const TimeInterval elapsed = std::max(time - timing.start_time, 0.0);
    const TimeInterval remaining = std::max(timing.end_time() - time, 0.0);
    const double linear_enter =
        enter_duration > 0 ? std::clamp(elapsed / enter_duration, 0.0, 1.0) : 1.0;
    const double linear_exit =
        exit_duration > 0 ? std::clamp(remaining / exit_duration, 0.0, 1.0) : 1.0;
    if (transition_style == 1) return std::min(linear_enter, linear_exit);
    return std::min(smoother_step(linear_enter), smoother_step(linear_exit));
}

FrameRenderPlan FrameSceneEvaluator::render_plan(TimeInterval presentation_time,
                                                 TimeInterval output_duration,
                                                 int frame_rate,
                                                 CompositionSize canvas_size,
                                                 bool has_cursor, bool has_spotlight,
                                                 bool camera_visible, bool has_stickers) {
    const TimeInterval safe_duration =
        std::isfinite(output_duration) ? std::max(output_duration, 0.0) : 0.0;
    const TimeInterval safe_time =
        std::isfinite(presentation_time)
            ? std::clamp(presentation_time, 0.0, safe_duration)
            : 0.0;

    FrameRenderPlan plan;
    plan.presentation_time = safe_time;
    plan.output_duration = safe_duration;
    plan.frame_rate = std::max(frame_rate, 1);
    plan.scene.time = safe_time;
    plan.scene.canvas_size = CompositionSize{
        .width = std::max(canvas_size.width, 2.0),
        .height = std::max(canvas_size.height, 2.0),
    };
    plan.scene.color = FrameColorContract::sdr_desktop();

    auto& order = plan.scene.layer_order;
    order.push_back(FrameLayerRole::kBackground);
    order.push_back(FrameLayerRole::kScreen);
    if (has_cursor) order.push_back(FrameLayerRole::kCursor);
    // 聚光是叠加在整幅基础画面之上的单一调暗效果，
    // 而不是分别处理壁纸与屏幕纹理。
    if (has_spotlight) order.push_back(FrameLayerRole::kSpotlight);
    if (camera_visible) order.push_back(FrameLayerRole::kCamera);
    if (has_stickers) order.push_back(FrameLayerRole::kStickers);
    return plan;
}

}  // namespace dogsc
