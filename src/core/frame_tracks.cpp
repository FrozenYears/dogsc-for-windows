#include "dogsc/frame_tracks.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "dogsc/project_model.hpp"

namespace dogsc {
namespace {
constexpr double kAdjacency = 1.0 / 120;
constexpr double kEndpoint = 1e-7;
double unit(double value) { return std::clamp(value, 0.0, 1.0); }
double mix(double a, double b, double p) { return a + (b - a) * p; }
NormalizedPoint mix(NormalizedPoint a, NormalizedPoint b, double p) {
    return {mix(a.x, b.x, p), mix(a.y, b.y, p)};
}
double angle(double a, double b, double p) {
    double delta = std::fmod(b - a, 360);
    if (delta > 180) delta -= 360;
    if (delta < -180) delta += 360;
    return a + delta * p;
}
template<class Clip> void order_motion(std::vector<Clip>& clips) {
    std::erase_if(clips, [](const auto& clip) {
        return !std::isfinite(clip.timing.start_time) ||
               !std::isfinite(clip.timing.duration) || clip.timing.duration <= 0;
    });
    std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
        return a.timing.start_time == b.timing.start_time ? a.id.bytes < b.id.bytes
                                                         : a.timing.start_time < b.timing.start_time;
    });
}
template<class Clip> std::optional<size_t> started(const std::vector<Clip>& clips, double time) {
    if (!std::isfinite(time)) return {};
    auto it = std::upper_bound(clips.begin(), clips.end(), time,
        [](double t, const auto& clip) { return t < clip.timing.start_time; });
    if (it == clips.begin()) return {};
    return static_cast<size_t>(it - clips.begin() - 1);
}
template<class Clip> bool touches_next(const std::vector<Clip>& clips, size_t index) {
    return index + 1 < clips.size() &&
        std::abs(clips[index + 1].timing.start_time - clips[index].timing.end_time()) <= kAdjacency;
}
double timing_progress(const TransitionTiming& timing, double time, bool returning,
                       const MotionStyle& motion) {
    const double duration = returning ? timing.return_duration
                                     : std::min(timing.lead_in_duration, timing.duration);
    const double offset = returning ? timing.return_progress_offset : timing.lead_in_progress_offset;
    const double local = duration > 1e-4 ? unit((time - (returning ? timing.end_time()
                                                        : timing.start_time)) / duration) : 1;
    return zoom_eased_progress(offset + (1 - offset) * local, timing.easing,
                               timing.custom_curve, motion);
}
CompositionPoint screen_offset(const ScreenMotionState& state, const ScreenAnchorViewport& v,
                               bool focus) {
    const double w = state.scale * v.fitted_width, h = state.scale * v.fitted_height;
    const double l = (v.border_width + v.decoration.left) * state.scale;
    const double r = (v.border_width + v.decoration.right) * state.scale;
    const double t = (v.border_width + v.decoration.top) * state.scale;
    const double b = (v.border_width + v.decoration.bottom) * state.scale;
    double cx, cy;
    if (!focus) {
        cx = state.position.x * (v.canvas_width - w - l - r) + w / 2 + l;
        cy = state.position.y * (v.canvas_height - h - t - b) + h / 2 + t;
    } else {
        const double fx = unit(state.position.x), fy = unit(state.position.y);
        const double recenter = unit((state.scale - 1) / 2) * 0.62;
        cx = mix(l + fx * std::max(v.canvas_width - l - r, 0.0),
                 (v.canvas_width + l - r) / 2, recenter) + (0.5 - fx) * w;
        cy = mix(t + fy * std::max(v.canvas_height - t - b, 0.0),
                 (v.canvas_height + t - b) / 2, recenter) + (0.5 - fy) * h;
    }
    return {cx - v.canvas_width / 2, cy - v.canvas_height / 2};
}
ScreenMotionSample screen_layout(const ScreenMotionState& state, const ScreenAnchorViewport& v,
                                 bool focus) {
    return {state, screen_offset(state, v, focus), state.position};
}
ScreenMotionSample screen_mix(const ScreenMotionState& a, const ScreenMotionState& b, double p,
                              const ScreenAnchorViewport& v, bool a_focus, bool b_focus,
                              NormalizedPoint anchor) {
    p = unit(p);
    ScreenMotionState state{mix(a.position, b.position, p), mix(a.scale, b.scale, p),
        angle(a.rotation_x, b.rotation_x, p), angle(a.rotation_y, b.rotation_y, p),
        angle(a.rotation_z, b.rotation_z, p), mix(a.perspective, b.perspective, p)};
    const auto oa = screen_offset(a, v, a_focus), ob = screen_offset(b, v, b_focus);
    return {state, {mix(oa.x, ob.x, p), mix(oa.y, ob.y, p)},
        a_focus && b_focus ? mix(a.position, anchor, p) : anchor};
}
CameraMotionSample camera_resolve(const CameraMotionState& state, double camera_aspect,
                                  double canvas_aspect) {
    CameraMotionSample s{state.position, state.size, 1, 0.5 * state.roundness, 0, state.opacity};
    if (state.layout.kind == CameraLayoutMode::Kind::kFullscreen) {
        s.aspect_ratio = std::max(canvas_aspect, 0.01);
        s.corner_fraction = 0;
        s.fullscreen_progress = 1;
    } else {
        switch (state.layout.shape) {
            case CameraShape::kSquare: break;
            case CameraShape::kCircle: s.corner_fraction = 0.5; break;
            case CameraShape::kHorizontal: s.aspect_ratio = 1.5; break;
            case CameraShape::kVertical: s.aspect_ratio = 0.72; break;
            case CameraShape::kOriginal: s.aspect_ratio = std::max(camera_aspect, 0.01); break;
        }
    }
    return s;
}
CameraMotionSample camera_mix(const CameraMotionSample& a, const CameraMotionSample& b, double p) {
    p = unit(p);
    if (b.opacity < 0.01) { auto s = a; s.opacity = mix(a.opacity, 0, p); return s; }
    if (a.opacity < 0.01) { auto s = b; s.opacity = mix(0, b.opacity, p); return s; }
    return {mix(a.position, b.position, p), mix(a.size, b.size, p),
        mix(a.aspect_ratio, b.aspect_ratio, p), mix(a.corner_fraction, b.corner_fraction, p),
        mix(a.fullscreen_progress, b.fullscreen_progress, p), mix(a.opacity, b.opacity, p)};
}
FrameStickerBackdropScene treatment(const StickerClip& c, double p) {
    return {c.hides_screen ? p : 0, c.hides_camera ? p : 0,
        c.hides_screen ? 0 : std::max(c.backdrop_blur, 0.0) * p,
        c.backdrop_blur_includes_camera ? std::max(c.backdrop_blur, 0.0) * p : 0};
}
void merge(FrameStickerBackdropScene& a, const FrameStickerBackdropScene& b) {
    a.screen_suppression = std::max(a.screen_suppression, b.screen_suppression);
    a.camera_suppression = std::max(a.camera_suppression, b.camera_suppression);
    a.screen_blur = std::max(a.screen_blur, b.screen_blur);
    a.camera_blur = std::max(a.camera_blur, b.camera_blur);
}
FrameStickerBackdropScene mix(FrameStickerBackdropScene a, FrameStickerBackdropScene b, double p) {
    return {mix(a.screen_suppression, b.screen_suppression, p),
        mix(a.camera_suppression, b.camera_suppression, p),
        mix(a.screen_blur, b.screen_blur, p), mix(a.camera_blur, b.camera_blur, p)};
}
void sticker_transform(FrameStickerScene& scene, StickerAnimationPreset preset, double progress) {
    const double p = unit(progress), travel = 1 - p;
    const double half = std::max(scene.width, 0.02) / 2;
    const double leading = std::min(-0.85, -half - 0.06);
    const double trailing = std::max(1.85, 1 + half + 0.06);
    const double l = (leading - scene.position.x) * travel;
    const double r = (trailing - scene.position.x) * travel;
    const double t = (leading - scene.position.y) * travel;
    const double b = (trailing - scene.position.y) * travel;
    switch (preset) {
        case StickerAnimationPreset::kNone: case StickerAnimationPreset::kFade: break;
        case StickerAnimationPreset::kPop: scene.scale = 0.72 + p * 0.28; break;
        case StickerAnimationPreset::kSlideLeft: scene.offset = {l, 0}; break;
        case StickerAnimationPreset::kSlideRight: scene.offset = {r, 0}; break;
        case StickerAnimationPreset::kSlideUp: scene.offset = {0, t}; break;
        case StickerAnimationPreset::kSlideDown: scene.offset = {0, b}; break;
        case StickerAnimationPreset::kSlideTopLeft: scene.offset = {l, t}; break;
        case StickerAnimationPreset::kSlideTopRight: scene.offset = {r, t}; break;
        case StickerAnimationPreset::kSlideBottomLeft: scene.offset = {l, b}; break;
        case StickerAnimationPreset::kSlideBottomRight: scene.offset = {r, b}; break;
    }
}
}  // namespace

double zoom_eased_progress(double value, ZoomEasingPreset easing, const ZoomBezierCurve& curve,
                           const MotionStyle& motion) {
    const double t = unit(value);
    if (easing != ZoomEasingPreset::kSpring)
        return (easing == ZoomEasingPreset::kCustom ? curve : ZoomBezierCurve::for_preset(easing)).value_at(t);
    if (t <= 0 || t >= 1) return t;
    const double m = std::max(motion.screen_spring_mass, 0.01);
    const double k = std::max(motion.screen_spring_stiffness, 0.01);
    const double d = std::max(motion.screen_spring_damping, 0.01);
    const double omega = std::sqrt(k / m), zeta = std::min(d / (2 * std::sqrt(k * m)), 0.9999);
    const double wd = omega * std::sqrt(1 - zeta * zeta);
    const auto response = [=](double seconds) {
        return 1 - std::exp(-zeta * omega * seconds) *
            (std::cos(wd * seconds) + zeta * omega / wd * std::sin(wd * seconds));
    };
    return unit(response(t * 0.72) / response(0.72));
}

ScreenMotionTrack::ScreenMotionTrack(std::vector<ScreenMotionClip> clips) : clips_(std::move(clips)) {
    order_motion(clips_);
}
const ScreenMotionClip* ScreenMotionTrack::active_clip(double time) const {
    const auto index = started(clips_, time);
    if (!index) return nullptr;
    const auto& c = clips_[*index];
    return c.timing.return_duration > 0 && time >= c.timing.effect_end_time() ? nullptr : &c;
}
ScreenMotionSample ScreenMotionTrack::sample(double time, const ScreenMotionState& base,
                                             const ScreenAnchorViewport& v, const MotionStyle& motion) const {
    const auto index = started(clips_, time);
    if (!index) return screen_layout(base, v, false);
    const auto& c = clips_[*index];
    if (time < c.timing.end_time()) {
        bool inherits = *index > 0 && (std::abs(clips_[*index - 1].timing.end_time() - c.timing.start_time)
            <= kAdjacency || clips_[*index - 1].timing.return_duration <= 1e-4);
        const auto& from = inherits ? clips_[*index - 1].target : base;
        return screen_mix(from, c.target, timing_progress(c.timing, time, false, motion), v,
                          inherits, true, c.target.position);
    }
    if (touches_next(clips_, *index) || c.timing.return_duration <= 1e-4)
        return screen_layout(c.target, v, true);
    return screen_mix(c.target, base, timing_progress(c.timing, time, true, motion), v,
                      true, false, c.target.position);
}
CameraMotionTrack::CameraMotionTrack(std::vector<CameraMotionClip> clips) : clips_(std::move(clips)) {
    order_motion(clips_);
}
CameraMotionSample CameraMotionTrack::sample(double time, const CameraMotionState& base,
                                             double camera_aspect, double canvas_aspect,
                                             const MotionStyle& motion) const {
    const auto initial = camera_resolve(base, camera_aspect, canvas_aspect);
    const auto index = started(clips_, time);
    if (!index) return initial;
    const auto& c = clips_[*index];
    const auto target = camera_resolve(c.target, camera_aspect, canvas_aspect);
    if (time < c.timing.end_time()) {
        bool inherits = *index > 0 && (std::abs(clips_[*index - 1].timing.end_time() - c.timing.start_time)
            <= kAdjacency || clips_[*index - 1].timing.return_duration <= 1e-4);
        const auto from = inherits ? camera_resolve(clips_[*index - 1].target, camera_aspect, canvas_aspect) : initial;
        return camera_mix(from, target, timing_progress(c.timing, time, false, motion));
    }
    if (touches_next(clips_, *index) || c.timing.return_duration <= 1e-4) return target;
    return camera_mix(target, initial, timing_progress(c.timing, time, true, motion));
}

ZoomAnimationTrack::ZoomAnimationTrack(const std::vector<ZoomClip>& clips, std::optional<double> duration) {
    auto ordered = std::make_shared<std::vector<ZoomClip>>(clips);
    std::erase_if(*ordered, [duration](const auto& c) {
        return !std::isfinite(c.start_time) || c.duration() <= 0.001 ||
            (duration && (!std::isfinite(*duration) || *duration <= 0 || c.start_time >= *duration));
    });
    std::sort(ordered->begin(), ordered->end(), [](const auto& a, const auto& b) {
        return a.start_time == b.start_time ? a.id.bytes < b.id.bytes : a.start_time < b.start_time;
    });
    if (duration) for (size_t i = 0; i < ordered->size(); ++i) {
        auto& c = (*ordered)[i];
        const double enter = std::max(c.preferred_enter_duration.value_or(c.enter_duration), 0.0);
        const double exit = std::max(c.preferred_exit_duration.value_or(c.exit_duration > 1e-4 ? c.exit_duration : 0.7), 0.0);
        if (i + 1 < ordered->size()) {
            const double gap = std::max((*ordered)[i + 1].start_time - c.end_time, 0.0);
            c.exit_duration = gap <= kAdjacency ? 0 : std::min(exit, gap);
        } else {
            const double available = std::max(*duration - c.start_time, 0.0);
            const double reserve = std::min(exit, std::max(available - std::min(enter, available * 0.5), 0.0));
            const double old_end = c.end_time;
            c.end_time = std::min(c.end_time, *duration - reserve);
            if (c.end_time < old_end - 1e-6) c.exit_progress_offset = 0;
            c.exit_duration = std::min(exit, std::max(*duration - c.end_time, 0.0));
        }
        c.enter_duration = std::min(enter, c.duration());
    }
    clips_ = std::move(ordered);
}
const ZoomClip* ZoomAnimationTrack::active_clip(double time) const {
    auto it = std::upper_bound(clips_->begin(), clips_->end(), time,
        [](double t, const auto& c) { return t < c.start_time; });
    while (it != clips_->begin()) {
        --it;
        if (time >= it->start_time && time <= it->effect_end_time()) return &*it;
    }
    return nullptr;
}
const ZoomClip* ZoomAnimationTrack::adjacent_previous(const ZoomClip& clip) const {
    auto it = std::find_if(clips_->begin(), clips_->end(), [&](const auto& c) { return c.id == clip.id; });
    if (it == clips_->begin() || it == clips_->end()) return nullptr;
    const auto& previous = *(it - 1);
    return std::abs(previous.end_time - clip.start_time) <= kAdjacency ? &previous : nullptr;
}
ZoomSample ZoomAnimationTrack::sample(double time, const MotionStyle& motion,
                                      std::optional<NormalizedPoint> automatic,
                                      std::optional<NormalizedPoint> inherited_focus, bool reanchor) const {
    const auto* c = active_clip(time);
    if (!c) return {};
    const auto* previous = adjacent_previous(*c);
    const auto focus = c->origin == ZoomKeyframeOrigin::kAutomatic ? automatic.value_or(c->focus) : c->focus;
    double enter = c->enter_duration;
    if (previous && enter <= 0.001) enter = motion.default_zoom_transition_duration;
    enter = std::min(enter, c->duration());
    double p;
    if (enter > 0.001 && time < c->start_time + enter) {
        const double offset = reanchor ? 0 : c->enter_progress_offset;
        p = zoom_eased_progress(offset + (1 - offset) * (time - c->start_time) / enter,
                                c->easing, c->custom_curve, motion);
    } else if (time <= c->end_time) p = 1;
    else if (c->exit_duration > 0.001)
        p = 1 - zoom_eased_progress(c->exit_progress_offset + (1 - c->exit_progress_offset) *
            (time - c->end_time) / c->exit_duration, c->easing, c->custom_curve, motion);
    else p = 0;
    if (previous && time <= c->end_time) {
        const auto from_focus = previous->origin == ZoomKeyframeOrigin::kAutomatic
            ? inherited_focus.value_or(previous->focus) : previous->focus;
        ZoomSample s{mix(previous->scale, c->scale, p), mix(from_focus, focus, p), 1,
                     mix(previous->scale, c->scale, p), {}};
        if (c->origin == ZoomKeyframeOrigin::kManual)
            s.manual_viewport_transition = ManualZoomViewportTransition{previous->scale, c->scale,
                                                                        from_focus, c->focus, p};
        return s;
    }
    return {mix(1, c->scale, p), focus, p, c->scale, {}};
}
NormalizedPoint ZoomViewportTransform::applying(NormalizedPoint p) const {
    return {0.5 + scale * (p.x - 0.5) + translation.x,
            0.5 + scale * (p.y - 0.5) + translation.y};
}
NormalizedPoint ZoomViewportTransform::unapplying(NormalizedPoint p) const {
    return {0.5 + (p.x - 0.5 - translation.x) / scale,
            0.5 + (p.y - 0.5 - translation.y) / scale};
}
ZoomViewportTransform ZoomViewportTransform::make(const ZoomSample& s, NormalizedCrop crop,
                                                  std::optional<CompositionRect> covering) {
    if (s.manual_viewport_transition) {
        const auto& t = *s.manual_viewport_transition;
        const auto from = make({t.from_scale, t.from_focus, 1, t.from_scale, {}}, crop, covering);
        const auto to = make({t.to_scale, t.to_focus, 1, t.to_scale, {}}, crop, covering);
        return {mix(from.scale, to.scale, unit(t.progress)), mix(from.translation, to.translation, unit(t.progress))};
    }
    const double scale = std::clamp(s.scale, 1.0, 6.0);
    crop = crop.clamped();
    const NormalizedPoint local{unit((s.focus.x - crop.x) / crop.width), unit((s.focus.y - crop.y) / crop.height)};
    const NormalizedPoint destination{std::clamp(local.x, 0.3, 0.7), std::clamp(local.y, 0.3, 0.7)};
    const auto desired = mix(local, destination, unit(s.framing_progress));
    NormalizedPoint translation{desired.x - (0.5 + scale * (local.x - 0.5)),
                                desired.y - (0.5 + scale * (local.y - 0.5))};
    if (covering) {
        translation.x = std::clamp(translation.x, covering->x + covering->width - 0.5 - scale / 2,
                                   covering->x - 0.5 + scale / 2);
        translation.y = std::clamp(translation.y, covering->y + covering->height - 0.5 - scale / 2,
                                   covering->y - 0.5 + scale / 2);
    }
    return {scale, translation};
}

StickerTransitionTrack::StickerTransitionTrack(std::vector<StickerClip> clips) {
    std::erase_if(clips, [](const auto& c) { return !std::isfinite(c.timing.start_time) ||
        !std::isfinite(c.timing.duration) || c.timing.duration <= 0; });
    std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
        return a.layer_index == b.layer_index ? a.id.bytes < b.id.bytes : a.layer_index < b.layer_index;
    });
    entries_.reserve(clips.size());
    struct Endpoint { double time; size_t index; bool start; };
    std::vector<Endpoint> endpoints;
    endpoints.reserve(clips.size() * 2);
    for (auto& c : clips) {
        const double enter = std::max(c.enter_duration, 0.0), exit = std::max(c.exit_duration, 0.0);
        const double factor = enter + exit > c.timing.duration ? c.timing.duration / (enter + exit) : 1;
        const size_t i = entries_.size();
        endpoints.push_back({c.timing.start_time, i, true});
        endpoints.push_back({c.timing.end_time(), i, false});
        entries_.push_back({std::move(c), enter * factor, exit * factor, {}, {}});
    }
    std::sort(endpoints.begin(), endpoints.end(), [](auto a, auto b) { return a.time < b.time; });
    const auto enter_budget = [](const Entry& e) { return e.clip.animation == StickerAnimationPreset::kNone ? 0 : e.enter_duration; };
    const auto exit_budget = [](const Entry& e) { return e.clip.exit_animation.value_or(e.clip.automatic_exit()) == StickerAnimationPreset::kNone ? 0 : e.exit_duration; };
    for (size_t cursor = 0; cursor < endpoints.size();) {
        const double boundary = endpoints[cursor].time;
        std::vector<size_t> outgoing, incoming;
        do {
            const auto e = endpoints[cursor++];
            (e.start ? incoming : outgoing).push_back(e.index);
        } while (cursor < endpoints.size() && endpoints[cursor].time - boundary <= kEndpoint);
        std::vector<size_t> shared;
        for (auto i : outgoing) if (std::find(incoming.begin(), incoming.end(), i) != incoming.end()) shared.push_back(i);
        const auto is_shared = [&](size_t i) { return std::find(shared.begin(), shared.end(), i) != shared.end(); };
        std::erase_if(outgoing, is_shared); std::erase_if(incoming, is_shared);
        if (outgoing.empty() || incoming.empty()) continue;
        double before = std::numeric_limits<double>::infinity(), after = before;
        double end_min = before, start_max = -before;
        FrameStickerBackdropScene out{}, in{};
        for (auto i : outgoing) {
            before = std::min(before, exit_budget(entries_[i]));
            end_min = std::min(end_min, entries_[i].clip.timing.end_time());
            merge(out, treatment(entries_[i].clip, 1));
        }
        for (auto i : incoming) {
            after = std::min(after, enter_budget(entries_[i]));
            start_max = std::max(start_max, entries_[i].clip.timing.start_time);
            merge(in, treatment(entries_[i].clip, 1));
        }
        const size_t join = joins_.size();
        joins_.push_back({end_min - before, start_max + after, out, in,
                         entries_[*std::max_element(incoming.begin(), incoming.end())].clip.animation_curve});
        for (auto i : outgoing) entries_[i].exit_join = join;
        for (auto i : incoming) entries_[i].entry_join = join;
    }
    std::vector<size_t> ordered(entries_.size());
    std::iota(ordered.begin(), ordered.end(), 0);
    std::sort(ordered.begin(), ordered.end(), [&](size_t a, size_t b) {
        const auto& x = entries_[a].clip.timing; const auto& y = entries_[b].clip.timing;
        return x.start_time == y.start_time ? x.end_time() < y.end_time() : x.start_time < y.start_time;
    });
    for (size_t i = 0; i < ordered.size(); ++i) for (size_t j = i + 1; j < ordered.size(); ++j) {
        const auto& a = entries_[ordered[i]]; const auto& b = entries_[ordered[j]];
        if (b.clip.timing.start_time >= a.clip.timing.end_time() - kEndpoint) break;
        if (b.clip.timing.end_time() <= a.clip.timing.end_time()) continue;
        const double start = a.clip.timing.end_time() - exit_budget(a);
        const double end = b.clip.timing.start_time + enter_budget(b);
        if (end <= start) continue;
        const auto x = treatment(a.clip, 1), y = treatment(b.clip, 1);
        FrameStickerBackdropScene shared{std::min(x.screen_suppression, y.screen_suppression),
            std::min(x.camera_suppression, y.camera_suppression), std::min(x.screen_blur, y.screen_blur),
            std::min(x.camera_blur, y.camera_blur)};
        bridges_.push_back({start, end, shared});
    }
}
StickerTrackSample StickerTransitionTrack::sample(double time, bool suppress_initial_entry) const {
    StickerTrackSample result;
    for (const auto& b : bridges_) if (time >= b.start && time < b.end) merge(result.backdrop, b.treatment);
    const auto join_progress = [&](const Join& j) { return j.end > j.start ? unit((time - j.start) / (j.end - j.start)) : (time >= j.end ? 1.0 : 0.0); };
    const auto contains = [&](const Join* j) { return j && time >= j->start && time < j->end; };
    for (const auto& j : joins_) if (contains(&j))
        merge(result.backdrop, mix(j.outgoing, j.incoming, element_motion_progress(join_progress(j), j.curve)));
    for (const auto& e : entries_) {
        const auto& c = e.clip;
        const Join* in = e.entry_join ? &joins_[*e.entry_join] : nullptr;
        const Join* out = e.exit_join ? &joins_[*e.exit_join] : nullptr;
        const bool joined = contains(in) || contains(out);
        if (!c.timing.contains(time) && !joined) continue;
        const double local = std::max(time - c.timing.start_time, 0.0);
        const double remaining = std::max(c.timing.end_time() - time, 0.0);
        const bool suppress = suppress_initial_entry && c.timing.start_time <= 1e-4;
        const double linear_in = in ? join_progress(*in) : (suppress || e.enter_duration <= 0 ? 1 : std::min(local / e.enter_duration, 1.0));
        const double linear_out = out ? 1 - join_progress(*out) : (e.exit_duration <= 0 ? 1 : std::min(remaining / e.exit_duration, 1.0));
        const double ep = element_motion_progress(linear_in, c.animation_curve);
        const double xp = element_motion_progress(linear_out, c.animation_curve);
        const auto exit_preset = c.exit_animation.value_or(c.automatic_exit());
        const double visibility = std::min(c.animation == StickerAnimationPreset::kNone ? 1 : ep,
                                          exit_preset == StickerAnimationPreset::kNone ? 1 : xp);
        const bool exiting = out ? time >= out->start : e.exit_duration > 0 && remaining < e.exit_duration;
        if (!joined) merge(result.backdrop, treatment(c, visibility));
        FrameStickerScene s;
        s.id = c.id; s.relative_path = c.relative_path; s.position = c.position;
        s.width = std::clamp(c.width, 0.02, 2.0);
        s.rotation_radians = c.rotation_degrees * 3.14159265358979323846 / 180;
        s.opacity = unit(c.opacity) * visibility;
        s.corner_radius = std::max(c.corner_radius, 0.0); s.border_width = std::max(c.border_width, 0.0);
        s.border_color = c.border_color; s.shadow_opacity = unit(c.shadow_opacity);
        s.shadow_radius = std::max(c.shadow_radius, 0.0); s.shadow_offset = {c.shadow_offset_x, c.shadow_offset_y};
        s.backdrop_blur = std::max(c.backdrop_blur, 0.0) * visibility;
        s.backdrop_blur_includes_camera = c.backdrop_blur_includes_camera;
        s.hides_screen = c.hides_screen; s.hides_camera = c.hides_camera;
        s.transition_progress = visibility; s.layer_index = c.layer_index;
        sticker_transform(s, exiting ? exit_preset : c.animation, exiting ? xp : ep);
        result.stickers.push_back(std::move(s));
    }
    return result;
}

}  // namespace dogsc
