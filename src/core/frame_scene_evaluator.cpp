#include "dogsc/frame_scene_evaluator.hpp"
#include "dogsc/project_model.hpp"


#include <algorithm>
#include <cmath>
#include <utility>

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

namespace {
double safe_aspect(double value) { return std::isfinite(value) ? std::max(value, 0.01) : 16.0 / 9.0; }
CompositionSize aspect_fit(double aspect, double width, double height) {
    aspect = safe_aspect(aspect);
    return width / std::max(height, 1.0) > aspect
        ? CompositionSize{height * aspect, height}
        : CompositionSize{width, width / aspect};
}
double mix_value(double a, double b, double p) { return a + (b - a) * p; }
CompositionPoint mix_point(CompositionPoint a, CompositionPoint b, double p) {
    return {mix_value(a.x, b.x, p), mix_value(a.y, b.y, p)};
}
NormalizedPoint mix_point(NormalizedPoint a, NormalizedPoint b, double p) {
    return {mix_value(a.x, b.x, p), mix_value(a.y, b.y, p)};
}
double normalized(double value) { return std::clamp(std::isfinite(value) ? value : 0.0, 0.0, 1.0); }
double transition_progress(const TransitionTiming& timing, double time, bool returning,
                           const MotionStyle& motion) {
    const double duration = returning ? timing.return_duration
        : std::min(timing.lead_in_duration, timing.duration);
    if (duration <= 0.0001) return 1;
    const double start = returning ? timing.end_time() : timing.start_time;
    const double offset = returning ? timing.return_progress_offset : timing.lead_in_progress_offset;
    const double linear = offset + (1 - offset) *
        std::clamp((time - start) / duration, 0.0, 1.0);
    return zoom_eased_progress(linear, timing.easing, timing.custom_curve, motion);
}
FrameShadow make_shadow(double opacity, double radius, CompositionPoint offset) {
    return FrameShadow{HexColor::black(), std::clamp(opacity, 0.0, 1.0),
                       std::max(radius, 0.0), offset};
}
FrameBackgroundSource make_background_source(const BackgroundSource& source) {
    return {source.kind, source.pattern_preset, source.dynamic_preset,
            source.relative_path, source.absolute_path};
}
FrameScreenChromeKind chrome_kind(ScreenFrameStyle style, double width, double height) {
    switch (style) {
        case ScreenFrameStyle::kBrowserLight:
        case ScreenFrameStyle::kBrowserDark: return FrameScreenChromeKind::kBrowser;
        case ScreenFrameStyle::kDevicePhone:
            return width >= height ? FrameScreenChromeKind::kDevicePhoneLandscape
                                    : FrameScreenChromeKind::kDevicePhonePortrait;
        case ScreenFrameStyle::kDevicePhonePortrait: return FrameScreenChromeKind::kDevicePhonePortrait;
        case ScreenFrameStyle::kDevicePhoneLandscape: return FrameScreenChromeKind::kDevicePhoneLandscape;
        case ScreenFrameStyle::kDeviceTablet:
            return width >= height ? FrameScreenChromeKind::kDeviceTabletLandscape
                                    : FrameScreenChromeKind::kDeviceTabletPortrait;
        case ScreenFrameStyle::kDeviceTabletPortrait: return FrameScreenChromeKind::kDeviceTabletPortrait;
        case ScreenFrameStyle::kDeviceTabletLandscape: return FrameScreenChromeKind::kDeviceTabletLandscape;
        case ScreenFrameStyle::kWindowLight:
        case ScreenFrameStyle::kWindowDark:
        case ScreenFrameStyle::kNone: return FrameScreenChromeKind::kWindow;
    }
    return FrameScreenChromeKind::kWindow;
}
bool dark_chrome(ScreenFrameStyle style) {
    return style == ScreenFrameStyle::kWindowDark || style == ScreenFrameStyle::kBrowserDark ||
           style == ScreenFrameStyle::kDevicePhone || style == ScreenFrameStyle::kDeviceTablet ||
           style == ScreenFrameStyle::kDevicePhonePortrait || style == ScreenFrameStyle::kDevicePhoneLandscape ||
           style == ScreenFrameStyle::kDeviceTabletPortrait || style == ScreenFrameStyle::kDeviceTabletLandscape;
}
std::optional<FrameScreenChromeScene> make_decoration(const CanvasStyle& canvas,
                                                      const ScreenFrameStyle style,
                                                      const FrameScreenScene& geometry,
                                                      double style_scale, double group_scale) {
    if (style == ScreenFrameStyle::kNone) return std::nullopt;
    const auto insets = ScreenFrameGeometry::decoration_insets_at_scale_one(
        style, canvas.screen_frame_scale, canvas.screen_frame_toolbar_scale,
        geometry.fitted_rect.width, geometry.fitted_rect.height, style_scale);
    const double motion_scale = std::max(group_scale, 0.0001);
    const double top = insets.top * motion_scale, right = insets.right * motion_scale;
    const double bottom = insets.bottom * motion_scale, left = insets.left * motion_scale;
    const CompositionRect outer{
        geometry.final_rect.x - left, geometry.final_rect.y - top,
        geometry.final_rect.width + left + right, geometry.final_rect.height + top + bottom};
    const double scale = std::max(style_scale * group_scale *
                                   std::clamp(canvas.screen_frame_scale, 0.6, 1.6), 0.0001);
    const CompositionRect toolbar{outer.x, outer.y, outer.width,
                                  top + std::min(2 * scale, top * 0.08)};
    const auto kind = chrome_kind(style, geometry.final_rect.width, geometry.final_rect.height);
    const bool device = kind != FrameScreenChromeKind::kWindow && kind != FrameScreenChromeKind::kBrowser;
    const bool phone = kind == FrameScreenChromeKind::kDevicePhonePortrait ||
                       kind == FrameScreenChromeKind::kDevicePhoneLandscape;
    const double default_outer = device
        ? std::min(outer.width, outer.height) * (phone ? 0.11 : 0.065) / scale
        : (kind == FrameScreenChromeKind::kBrowser ? 18.0 : 14.0);
    const double outer_radius = std::min(
        std::min(std::max(canvas.screen_frame_outer_corner_radius.value_or(default_outer), 0.0) * scale,
                 std::min(outer.width, outer.height) / 2), 160.0);
    const double default_content = device
        ? std::max(outer_radius - std::min(top, left), 0.0) / scale
        : (kind == FrameScreenChromeKind::kBrowser ? 8.0 : 10.0);
    const double content_radius = std::min(
        std::max(canvas.screen_frame_content_corner_radius.value_or(default_content), 0.0) * scale,
        std::min(geometry.final_rect.width, geometry.final_rect.height) / 2);
    const bool dark = dark_chrome(style);
    return FrameScreenChromeScene{
        kind, outer, toolbar, geometry.final_rect, outer_radius, content_radius,
        dark ? HexColor{"#222327"} : HexColor{"#ECEDF0"},
        dark ? HexColor{"#313238"} : HexColor{"#F4F4F6"},
        dark ? HexColor{"#4B4D55"} : HexColor{"#C9CACE"},
        dark ? HexColor{"#1B1C20"} : HexColor{"#FFFFFF"},
        dark ? HexColor{"#A9ABB3"} : HexColor{"#7A7C84"},
        canvas.screen_frame_title, canvas.screen_frame_browser_address};
}
std::optional<FrameFocusScene> focus_scene(const ZoomClip* zoom, const ScreenMotionClip* screen,
                                          NormalizedPoint zoom_focus, NormalizedCrop crop,
                                          std::optional<NormalizedPoint> pointer, double time,
                                          const MotionStyle& motion) {
    FocusEffect effect;
    NormalizedPoint anchor{};
    const TransitionTiming* timing = nullptr;
    TransitionTiming zoom_timing;
    bool holds = false;
    if (screen && screen->focus_effect) {
        effect = *screen->focus_effect;
        if (!effect.shape) { effect.shape = FocusEffectShape::kLinear; effect.target = FocusEffectTarget::kFixed; effect.dimming = 0; }
        anchor = {crop.x + screen->target.position.x * crop.width,
                  crop.y + screen->target.position.y * crop.height};
        timing = &screen->timing; holds = timing->return_duration == 0;
    } else if (zoom && zoom->focus_effect) {
        effect = *zoom->focus_effect; anchor = zoom_focus;
        zoom_timing = TransitionTiming{};
        zoom_timing.start_time = zoom->start_time;
        zoom_timing.duration = zoom->duration();
        zoom_timing.lead_in_duration = zoom->enter_duration;
        zoom_timing.lead_in_progress_offset = zoom->enter_progress_offset;
        zoom_timing.easing = zoom->easing;
        zoom_timing.custom_curve = zoom->custom_curve;
        zoom_timing.return_duration = zoom->exit_duration;
        zoom_timing.return_progress_offset = zoom->exit_progress_offset;
        zoom_timing.preferred_lead_in_duration = zoom->preferred_enter_duration;
        zoom_timing.preferred_return_duration = zoom->preferred_exit_duration;
        timing = &zoom_timing;
    } else return std::nullopt;
    if (!effect.is_valid() || (effect.blur <= 0 && effect.dimming <= 0) || !timing) return std::nullopt;
    const double amount = time < timing->start_time ? 0 : time < timing->end_time()
        ? zoom_eased_progress(timing->lead_in_duration > 0
            ? timing->lead_in_progress_offset + (1 - timing->lead_in_progress_offset) *
                std::clamp((time - timing->start_time) / std::min(timing->lead_in_duration, timing->duration), 0.0, 1.0)
            : 1, timing->easing, timing->custom_curve, motion)
        : timing->return_duration > 0
            ? 1 - zoom_eased_progress(timing->return_progress_offset +
                (1 - timing->return_progress_offset) * std::clamp((time - timing->end_time()) / timing->return_duration, 0.0, 1.0),
                timing->easing, timing->custom_curve, motion)
            : (holds ? 1 : 0);
    if (amount <= 0) return std::nullopt;
    const NormalizedPoint center = effect.target == FocusEffectTarget::kPointer
        ? pointer.value_or(anchor) : effect.target == FocusEffectTarget::kFixed ? effect.center : anchor;
    return FrameFocusScene{effect, {normalized(center.x), normalized(center.y)}, normalized(amount)};
}
}  // namespace

FrameScene FrameSceneEvaluator::scene(const RecorderProject& project, TimeInterval time,
                                      CompositionSize canvas_size, const FrameEvaluationInputs& inputs,
                                      std::optional<TimeInterval> output_duration,
                                      FrameColorContract color) {
    const double width = std::max(std::isfinite(canvas_size.width) ? canvas_size.width : 0.0, 2.0);
    const double height = std::max(std::isfinite(canvas_size.height) ? canvas_size.height : 0.0, 2.0);
    const double sample_time = std::isfinite(time) ? std::max(time, 0.0) : 0.0;
    const NormalizedCrop crop = project.canvas.crop.clamped();
    constexpr double tolerance = 1e-5;
    const double style_scale = std::max(std::min(width, height), 1.0) / 1080.0;
    const double source_aspect = inputs.screen_source_size.width > 0 && inputs.screen_source_size.height > 0
        ? inputs.screen_source_size.width / inputs.screen_source_size.height
        : inputs.source_aspect_ratio;
    const double cropped_aspect = std::max(safe_aspect(source_aspect) * crop.width / crop.height, 0.01);
    const double padding = std::min(std::max(project.canvas.padding * style_scale, 0.0),
                                    std::min(width, height) * 0.42);
    const auto fitted_size = aspect_fit(cropped_aspect, std::max(width - 2 * padding, 2.0),
                                        std::max(height - 2 * padding, 2.0));
    const CompositionRect fitted{(width - fitted_size.width) / 2, (height - fitted_size.height) / 2,
                                 fitted_size.width, fitted_size.height};
    const double border_at_scale = std::min(std::max(project.canvas.border_width, 0.0), 60.0) * style_scale;
    const ScreenFrameStyle frame_style = screen_frame_style_from_raw(project.canvas.screen_frame);
    const auto decoration_insets = ScreenFrameGeometry::decoration_insets_at_scale_one(
        frame_style, project.canvas.screen_frame_scale, project.canvas.screen_frame_toolbar_scale,
        fitted.width, fitted.height, style_scale);
    ScreenAnchorViewport viewport{width, height, fitted.width, fitted.height, border_at_scale, decoration_insets};
    const ScreenMotionState base_screen{project.canvas.content_position, project.canvas.content_scale, 0, 0, 0, 0.65};
    ScreenMotionTrack local_screen(project.screen_motion_clips);
    const auto& screen_track = inputs.screen_motion_track ? *inputs.screen_motion_track : local_screen;
    const auto screen_sample = screen_track.sample(sample_time, base_screen, viewport, project.motion);
    const auto& motion = screen_sample.state;
    const double manual_scale = std::clamp(std::isfinite(motion.scale) ? motion.scale : 1.0, 0.2, 4.0);
    const CompositionPoint manual_offset = screen_sample.manual_offset;
    const double scaled_width = fitted.width * manual_scale, scaled_height = fitted.height * manual_scale;
    const CompositionRect base_rect{
        fitted.mid_x() - scaled_width / 2 + manual_offset.x,
        fitted.mid_y() - scaled_height / 2 + manual_offset.y,
        scaled_width, scaled_height};
    ZoomAnimationTrack local_zoom(project.zoom_clips, output_duration);
    const auto& zoom_track = inputs.zoom_track ? *inputs.zoom_track : local_zoom;
    const auto* active_zoom = zoom_track.active_clip(sample_time);
    const bool reanchors_at_cut = active_zoom && inputs.pointer_track &&
        inputs.pointer_track->automatic_entry_starts_after_cut(*active_zoom);
    std::optional<NormalizedPoint> automatic_focus;
    if (active_zoom && active_zoom->origin == ZoomKeyframeOrigin::kAutomatic && inputs.pointer_track)
        automatic_focus = inputs.pointer_track->automatic_camera_focus(
            *active_zoom, sample_time, project.motion, reanchors_at_cut);
    std::optional<NormalizedPoint> inherited_focus;
    if (active_zoom) if (const auto* previous = zoom_track.adjacent_previous(*active_zoom)) {
        if (previous->origin == ZoomKeyframeOrigin::kAutomatic && inputs.pointer_track)
            inherited_focus = inputs.pointer_track->automatic_camera_focus(
                *previous, previous->end_time, project.motion);
    }
    const auto zoom = zoom_track.sample(sample_time, project.motion, automatic_focus,
                                        inherited_focus, reanchors_at_cut);
    const bool covered = frame_style == ScreenFrameStyle::kNone &&
        !screen_track.active_clip(sample_time) && base_rect.x <= tolerance && base_rect.y <= tolerance &&
        base_rect.x + base_rect.width >= width - tolerance && base_rect.y + base_rect.height >= height - tolerance;
    const std::optional<CompositionRect> visible = covered
        ? std::optional<CompositionRect>{{-base_rect.x / base_rect.width, -base_rect.y / base_rect.height,
                                          width / base_rect.width, height / base_rect.height}}
        : std::nullopt;
    const auto zoom_view = ZoomViewportTransform::make(zoom, crop, visible);
    const double decoration_growth = manual_scale * (zoom_view.scale - 1) / 2;
    CompositionRect final_rect{
        base_rect.mid_x() - base_rect.width * zoom_view.scale / 2 + zoom_view.translation.x * base_rect.width +
            (decoration_insets.left - decoration_insets.right) * decoration_growth,
        base_rect.mid_y() - base_rect.height * zoom_view.scale / 2 + zoom_view.translation.y * base_rect.height +
            (decoration_insets.top - decoration_insets.bottom) * decoration_growth,
        base_rect.width * zoom_view.scale, base_rect.height * zoom_view.scale};
    const auto opening_elements = project.opening_sequence.participating_elements(
        inputs.camera_source_size.has_value() && !project.camera.is_hidden,
        std::any_of(project.sticker_clips.begin(), project.sticker_clips.end(), [](const auto& c) {
            return c.timing.start_time <= 0.0001 && c.timing.end_time() > 0.0001;
        }));
    const auto screen_opening = opening_sample(project.opening_sequence, OpeningSequenceElement::kScreen,
                                               opening_elements, sample_time, width, height);
    final_rect = opening_rect(final_rect, screen_opening);
    const double final_radius = covered ? 0 : std::max(project.canvas.corner_radius, 0.0) * style_scale *
        manual_scale * zoom_view.scale * screen_opening.scale;
    const double final_border = border_at_scale * zoom_view.scale * screen_opening.scale;
    const CompositionPoint projection_anchor{
        final_rect.x + final_rect.width * screen_sample.projection_anchor.x,
        final_rect.y + final_rect.height * screen_sample.projection_anchor.y};
    const auto projection_rect = frame_style == ScreenFrameStyle::kNone ? final_rect : CompositionRect{
        final_rect.x - decoration_insets.left * manual_scale * zoom_view.scale * screen_opening.scale,
        final_rect.y - decoration_insets.top * manual_scale * zoom_view.scale * screen_opening.scale,
        final_rect.width + (decoration_insets.left + decoration_insets.right) * manual_scale * zoom_view.scale * screen_opening.scale,
        final_rect.height + (decoration_insets.top + decoration_insets.bottom) * manual_scale * zoom_view.scale * screen_opening.scale};
    const auto projected = ScreenProjection::project(projection_rect,
        std::clamp(motion.rotation_x + screen_opening.rotation_x, -89.0, 89.0),
        std::clamp(motion.rotation_y + screen_opening.rotation_y, -89.0, 89.0), motion.rotation_z,
        std::clamp(motion.perspective + screen_opening.perspective, 0.0, 2.0), projection_anchor);
    FrameScreenScene screen{
        crop, fitted, base_rect, final_rect, projected, final_radius, final_border,
        project.canvas.border_color, std::clamp(project.canvas.inset_opacity, 0.0, 1.0),
        project.canvas.shadow_strength > 0
            ? std::optional<FrameShadow>(make_shadow(project.canvas.shadow_strength,
                22 * style_scale * manual_scale * zoom_view.scale * screen_opening.scale,
                {0, 12 * style_scale * manual_scale * zoom_view.scale * screen_opening.scale}))
            : std::nullopt,
        std::nullopt, {}, std::nullopt, screen_opening.opacity};
    screen.decoration = make_decoration(project.canvas, frame_style, screen,
        style_scale, manual_scale * zoom_view.scale * screen_opening.scale);
    screen.mosaics.reserve(project.mosaic_clips.size());
    for (const auto& clip : project.mosaic_clips) if (clip.timing.contains(sample_time)) {
        screen.mosaics.push_back({clip.source_rect, clip.corner_radius, clip.style, clip.intensity,
            clip.spotlight_dimming, overlay_effect_progress(
                clip.timing, static_cast<int>(clip.transition_style), clip.transition_in_duration,
                clip.transition_out_duration, sample_time)});
    }
    const auto* active_screen = screen_track.active_clip(sample_time);
    std::optional<NormalizedPoint> pointer_position;
    if (inputs.pointer_evaluation) pointer_position = inputs.pointer_evaluation->position;
    else if (inputs.pointer_track) pointer_position = inputs.pointer_track->position(sample_time, project.motion);
    screen.focus_effect = focus_scene(active_zoom, active_screen, zoom.focus, crop,
                                      pointer_position, sample_time, project.motion);
    const auto camera_sample = inputs.camera_source_size ? [&]() {
        CameraMotionTrack local_camera(project.camera_motion_clips);
        const auto& track = inputs.camera_motion_track ? *inputs.camera_motion_track : local_camera;
        const CameraMotionState base{CameraLayoutMode::shaped(project.camera.shape), project.camera.position,
                                     project.camera.size, project.camera.roundness, 1};
        return track.sample(sample_time, base,
            inputs.camera_source_size->width / std::max(inputs.camera_source_size->height, 1.0), width / height,
            project.motion);
    }() : CameraMotionSample{};
    std::optional<FrameCameraScene> camera;
    if (inputs.camera_source_size && !project.camera.is_hidden) {
        const double zoom_progress = std::clamp((zoom.scale - 1) / 0.565, 0.0, 1.0);
        const double zoom_scale = 1 + (project.camera.scale_during_zoom - 1) * zoom_progress;
        const double h = std::min(std::max(std::min(width, height) * camera_sample.size * zoom_scale,
                                           72 * style_scale), std::min(width, height) * 1.2);
        const double w = h * std::max(camera_sample.aspect_ratio, 0.01);
        const double fit = std::min({1.0, width * 1.2 / std::max(w, 0.0001), height * 1.2 / std::max(h, 0.0001)});
        const CompositionRect shaped{normalized(camera_sample.position.x) * (width - w * fit),
                                     normalized(camera_sample.position.y) * (height - h * fit), w * fit, h * fit};
        const double full = normalized(camera_sample.fullscreen_progress);
        const CompositionRect rect{mix_value(shaped.x, 0, full), mix_value(shaped.y, 0, full),
                                    mix_value(shaped.width, width, full), mix_value(shaped.height, height, full)};
        const auto camera_opening = opening_sample(project.opening_sequence, OpeningSequenceElement::kCamera,
                                                   opening_elements, sample_time, width, height);
        const auto opened_rect = opening_rect(rect, camera_opening);
        const bool mirrored = project.camera.is_mirrored;
        const NormalizedPoint camera_focus{mirrored ? 1 - project.camera.content_position.x : project.camera.content_position.x,
                                           project.camera.content_position.y};
        camera = FrameCameraScene{opened_rect,
            CameraContentFill::layout(inputs.camera_source_size->width, inputs.camera_source_size->height,
                                      opened_rect, camera_focus, project.camera.content_scale),
            std::min(opened_rect.width, opened_rect.height) * std::clamp(camera_sample.corner_fraction, 0.0, 0.5) * camera_opening.scale,
            std::min(std::max(project.camera.border_width, 0.0), 18.0) * style_scale * (1 - full) * camera_opening.scale,
            HexColor::white(), project.camera.shadow_strength > 0
                ? std::optional<FrameShadow>(make_shadow(project.camera.shadow_strength, 14 * style_scale, {0, 8 * style_scale}))
                : std::nullopt,
            camera_sample.opacity * camera_opening.opacity, mirrored};
    }
    std::optional<PointerSample> pointer_sample;
    if (inputs.pointer_evaluation) pointer_sample = inputs.pointer_evaluation->cursor;
    else if (inputs.pointer_track)
        pointer_sample = inputs.pointer_track->evaluation(sample_time, project.motion,
            project.cursor_style, active_zoom && active_zoom->origin == ZoomKeyframeOrigin::kAutomatic
                ? active_zoom : nullptr).cursor;
    std::optional<FrameCursorScene> cursor;
    if (pointer_sample) {
        const auto asset_id = project.cursor_style.asset_id == CursorAssetID::automatic()
            ? pointer_sample->recorded_cursor_asset_id.value_or(CursorAssetID::system_arrow())
            : project.cursor_style.asset_id;
        const CursorAssetMetrics* metrics = inputs.cursor_metrics;
        if (inputs.cursor_metrics_by_asset) for (const auto& item : *inputs.cursor_metrics_by_asset)
            if (item.first == asset_id) { metrics = &item.second; break; }
        if (metrics) {
            const NormalizedPoint local{(pointer_sample->location.x - crop.x) / crop.width,
                                        (pointer_sample->location.y - crop.y) / crop.height};
            if (local.x >= 0 && local.x <= 1 && local.y >= 0 && local.y <= 1) {
                const CompositionPoint point{final_rect.x + local.x * final_rect.width,
                                             final_rect.y + local.y * final_rect.height};
                CursorRenderLayout layout{};
                if (CursorRenderGeometry::layout(point.x, point.y, std::min(width, height),
                                                  project.cursor_style.size, *metrics, layout)) {
                    const auto click_geometry = pointer_sample->click_phase
                        ? CursorRenderGeometry::click_geometry(
                            project.cursor_style.click_effect_style,
                            pointer_sample->click_phase->progress,
                            layout.click_effect_height,
                            project.cursor_style.click_opacity,
                            project.cursor_style.click_scale)
                        : std::nullopt;
                    cursor = FrameCursorScene{asset_id, *metrics, local, layout, pointer_sample->is_clicking,
                        pointer_sample->click_phase, click_geometry, project.cursor_style.click_effect_style,
                        project.cursor_style.click_color, project.cursor_style.click_opacity,
                        project.cursor_style.click_scale, pointer_sample->rotation_radians,
                        std::optional<FrameShadow>(make_shadow(0.42,
                            std::max(layout.click_effect_height * 1.5 / 44.0, 0.5),
                            {0, std::max(layout.click_effect_height / 44.0, 0.5)})),
                        FrameCursorAttachment::kScreen};
                }
            }
        }
    }
    StickerTransitionTrack local_stickers(project.sticker_clips);
    const auto& sticker_track = inputs.sticker_track ? *inputs.sticker_track : local_stickers;
    auto sticker_sample = sticker_track.sample(sample_time,
        project.opening_sequence.is_enabled && std::find(project.opening_sequence.included_elements.begin(),
            project.opening_sequence.included_elements.end(), OpeningSequenceElement::kStickers) !=
            project.opening_sequence.included_elements.end());
    const auto sticker_opening = opening_sample(project.opening_sequence, OpeningSequenceElement::kStickers,
                                                opening_elements, sample_time, width, height);
    for (auto& sticker : sticker_sample.stickers) {
        sticker.offset.x += sticker_opening.offset.x / width;
        sticker.offset.y += sticker_opening.offset.y / height;
        sticker.scale *= sticker_opening.scale;
        sticker.opacity *= sticker_opening.opacity;
    }
    FrameScene result;
    result.time = sample_time;
    result.canvas_size = {width, height};
    result.color = color;
    result.background = {make_background_source(project.canvas.background_source),
                          std::max(project.canvas.background_blur * style_scale, 0.0),
                          std::max(project.canvas.pattern_scale, 0.1),
                          normalized(project.canvas.pattern_opacity)};
    result.screen = std::move(screen);
    result.camera = std::move(camera);
    result.cursor = std::move(cursor);
    result.stickers = std::move(sticker_sample.stickers);
    result.sticker_backdrop = sticker_sample.backdrop;
    result.layer_order = {FrameLayerRole::kBackground, FrameLayerRole::kScreen};
    if (result.cursor) result.layer_order.push_back(FrameLayerRole::kCursor);
    if (result.screen.focus_effect || std::any_of(result.screen.mosaics.begin(), result.screen.mosaics.end(),
        [](const auto& m) { return m.style == MosaicEffectStyle::kSpotlight && m.transition_progress > 0; }))
        result.layer_order.push_back(FrameLayerRole::kSpotlight);
    if (result.camera && result.camera->opacity > 0) result.layer_order.push_back(FrameLayerRole::kCamera);
    if (!result.stickers.empty()) result.layer_order.push_back(FrameLayerRole::kStickers);
    return result;
}

FrameRenderPlan FrameSceneEvaluator::render_plan(const RecorderProject& project,
                                                 TimeInterval presentation_time,
                                                 TimeInterval output_duration, int frame_rate,
                                                 CompositionSize canvas_size,
                                                 const FrameEvaluationInputs& inputs,
                                                 FrameColorContract color) {
    const double duration = std::isfinite(output_duration) ? std::max(output_duration, 0.0) : 0.0;
    const double time = std::isfinite(presentation_time)
        ? std::clamp(presentation_time, 0.0, duration) : 0.0;
    FrameRenderPlan plan;
    plan.presentation_time = time;
    plan.output_duration = duration;
    plan.frame_rate = std::max(frame_rate, 1);
    plan.scene = scene(project, time, canvas_size, inputs, duration, color);
    return plan;
}

}  // namespace dogsc
