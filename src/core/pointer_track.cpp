#include "dogsc/pointer_track.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "dogsc/project_model.hpp"

namespace dogsc {
namespace {
double unit(double x) { return std::clamp(x, 0.0, 1.0); }
double smoother(double x) { x = unit(x); return x*x*x*(x*(x*6-15)+10); }
NormalizedPoint blend(NormalizedPoint a, NormalizedPoint b, double p) {
    return {a.x + (b.x-a.x)*p, a.y + (b.y-a.y)*p};
}
bool click_event(const PointerEventRecord& e) { return e.kind == PointerEventKind::kLeftClick || e.kind == PointerEventKind::kRightClick; }
bool continuous(const PointerEventRecord& a, const PointerEventRecord& b) {
    const double dt = b.time-a.time;
    return dt > 0 && (dt <= 0.12 || (dt <= 0.5 &&
        std::hypot(b.location.x-a.location.x, b.location.y-a.location.y) <= 0.03));
}
double click_duration(CursorClickEffectStyle style) {
    switch (style) {
        case CursorClickEffectStyle::kRipple: return 0.36;
        case CursorClickEffectStyle::kGlow: return 0.32;
        case CursorClickEffectStyle::kPulse: return 0.38;
        case CursorClickEffectStyle::kBurst: return 0.22;
        case CursorClickEffectStyle::kNone: return 0;
    }
    return 0;
}
std::optional<PointerEventRecord> raw_interpolate(const std::vector<PointerEventRecord>& events, double time) {
    if (events.empty()) return {};
    auto upper=std::upper_bound(events.begin(),events.end(),time,[](double t,const auto& e){ return t<e.time; });
    if (upper==events.begin()) return events.front();
    if (upper==events.end()) return events.back();
    const auto& b=*upper; const auto& a=*(upper-1);
    const double p=unit((time-a.time)/std::max(b.time-a.time,1e-12));
    auto result=a; result.time=time; result.location=blend(a.location,b.location,p); result.kind=PointerEventKind::kMove;
    return result;
}
}  // namespace

PointerTrack::PointerTrack(std::vector<PointerEventRecord> events)
    : PointerTrack(std::move(events), MotionStyle{}) {}
PointerTrack::PointerTrack(std::vector<PointerEventRecord> events, const MotionStyle& motion)
    : events_(std::move(events)), spring_cache_(std::make_shared<SpringCache>()) {
    std::erase_if(events_, [](const auto& e) { return !std::isfinite(e.time) || e.time < 0 ||
        !std::isfinite(e.location.x) || !std::isfinite(e.location.y); });
    std::stable_sort(events_.begin(), events_.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    for (auto& e : events_) { e.location.x = unit(e.location.x); e.location.y = unit(e.location.y); }
    (void)motion;
}
size_t PointerTrack::upper_index(double time) const {
    return static_cast<size_t>(std::upper_bound(events_.begin(), events_.end(), time,
        [](double t, const auto& e) { return t < e.time; }) - events_.begin());
}
NormalizedPoint PointerTrack::interpolated(size_t lower_index, size_t upper_index, double time,
                                           const MotionStyle& motion) const {
    const auto& lower = events_[lower_index];
    const auto& upper = events_[upper_index];
    const double duration = upper.time - lower.time;
    if (duration <= 0 || !continuous(lower, upper)) return lower.location;
    const double progress = unit((time - lower.time) / duration);
    if (motion.cursor == CursorMotionStyle::kNone) {
        return {lower.location.x + (upper.location.x - lower.location.x) * progress,
                lower.location.y + (upper.location.y - lower.location.y) * progress};
    }
    const double tangent_scale = motion.cursor == CursorMotionStyle::kMedium ? 0.78 : 1.0;
    const auto tangent = [&](size_t index) {
        size_t previous = index - 1;
        while (previous < index && events_[previous].time >= events_[index].time) {
            if (previous == 0) { previous = events_.size(); break; }
            --previous;
        }
        size_t next = index + 1;
        while (next < events_.size() && events_[next].time <= events_[index].time) ++next;
        if (previous < index && next < events_.size()) {
            const auto& before = events_[previous];
            const auto& current = events_[index];
            const auto& after = events_[next];
            const double previous_duration = current.time - before.time;
            const double next_duration = after.time - current.time;
            const auto axis = [&](double previous_value, double value, double next_value) {
                const double previous_slope = (value - previous_value) / previous_duration;
                const double next_slope = (next_value - value) / next_duration;
                if (!std::isfinite(previous_slope) || !std::isfinite(next_slope) ||
                    previous_slope * next_slope <= 0) return 0.0;
                const double first_weight = 2 * next_duration + previous_duration;
                const double second_weight = next_duration + 2 * previous_duration;
                const double denominator = first_weight / previous_slope + second_weight / next_slope;
                if (!std::isfinite(denominator) || std::abs(denominator) <= std::numeric_limits<double>::min()) return 0.0;
                return (first_weight + second_weight) / denominator;
            };
            return NormalizedPoint{
                axis(before.location.x, current.location.x, after.location.x),
                axis(before.location.y, current.location.y, after.location.y)};
        }
        if (next < events_.size()) {
            const double seconds = events_[next].time - events_[index].time;
            if (seconds <= 0) return NormalizedPoint{};
            return NormalizedPoint{(events_[next].location.x - events_[index].location.x) / seconds,
                                   (events_[next].location.y - events_[index].location.y) / seconds};
        }
        if (previous < index) {
            const double seconds = events_[index].time - events_[previous].time;
            if (seconds <= 0) return NormalizedPoint{};
            return NormalizedPoint{(events_[index].location.x - events_[previous].location.x) / seconds,
                                   (events_[index].location.y - events_[previous].location.y) / seconds};
        }
        return NormalizedPoint{};
    };
    const auto lower_tangent = tangent(lower_index);
    const auto upper_tangent = tangent(upper_index);
    const double t2 = progress * progress, t3 = t2 * progress;
    const double h00 = 2 * t3 - 3 * t2 + 1;
    const double h10 = t3 - 2 * t2 + progress;
    const double h01 = -2 * t3 + 3 * t2;
    const double h11 = t3 - t2;
    return {unit(h00 * lower.location.x + h10 * duration * lower_tangent.x * tangent_scale +
                 h01 * upper.location.x + h11 * duration * upper_tangent.x * tangent_scale),
            unit(h00 * lower.location.y + h10 * duration * lower_tangent.y * tangent_scale +
                 h01 * upper.location.y + h11 * duration * upper_tangent.y * tangent_scale)};
}
NormalizedPoint PointerTrack::sample_spring(const std::vector<SpringSample>& samples, double time) const {
    if (time <= samples.front().time) return samples.front().location;
    if (time >= samples.back().time) return samples.back().location;
    const auto upper = std::upper_bound(samples.begin(), samples.end(), time,
        [](double t, const auto& sample) { return t < sample.time; });
    const auto& lower = *(upper - 1);
    return blend(lower.location, upper->location,
                 unit((time - lower.time) / (upper->time - lower.time)));
}
NormalizedPoint PointerTrack::spring_position(double time, const MotionStyle& motion) const {
    const double mass = std::max(motion.cursor_spring_mass, 0.01);
    const double stiffness = std::max(motion.cursor_spring_stiffness, 0.01);
    const double damping = std::max(motion.cursor_spring_damping, 0.01);
    std::lock_guard lock(spring_cache_->mutex);
    ++spring_cache_->clock;
    for (auto& variant : spring_cache_->variants) {
        if (variant.mass == mass && variant.stiffness == stiffness && variant.damping == damping) {
            variant.last_used = spring_cache_->clock;
            return sample_spring(variant.samples, time);
        }
    }
    auto samples = build_spring(motion);
    if (spring_cache_->variants.size() == 2) {
        const auto oldest = std::min_element(spring_cache_->variants.begin(), spring_cache_->variants.end(),
            [](const auto& a, const auto& b) { return a.last_used < b.last_used; });
        spring_cache_->variants.erase(oldest);
    }
    spring_cache_->variants.push_back({mass, stiffness, damping, spring_cache_->clock, std::move(samples)});
    return sample_spring(spring_cache_->variants.back().samples, time);
}
size_t PointerTrack::cached_spring_variant_count() const {
    std::lock_guard lock(spring_cache_->mutex);
    return spring_cache_->variants.size();
}
std::optional<NormalizedPoint> PointerTrack::resolved(double time, const MotionStyle& motion) const {
    if (!std::isfinite(time) || events_.empty() || time < events_.front().time) return {};
    if (motion.cursor == CursorMotionStyle::kSmooth) return spring_position(time, motion);
    const size_t upper = upper_index(time);
    const size_t lower = upper > 0 ? upper - 1 : 0;
    return upper < events_.size() ? interpolated(lower, upper, time, motion)
                                  : events_[lower].location;
}
std::optional<NormalizedPoint> PointerTrack::position(double time,const MotionStyle& motion) const {
    return resolved(time,motion);
}
PointerTrackEvaluation PointerTrack::evaluation(double time,const MotionStyle& motion,
                                                 const CursorStyle& style) const {
    PointerTrackEvaluation out;
    out.position=resolved(time,motion);
    if (!out.position) return out;
    const size_t upper=upper_index(time), lower=upper ? upper-1 : 0;
    if (style.asset_id==CursorAssetID::hidden() ||
        (style.hide_when_idle && time-events_[lower].time>style.idle_delay)) return out;
    PointerSample sample;
    sample.location=*out.position;
    sample.recorded_cursor_asset_id=events_[lower].cursor_asset_id;
    const double duration=click_duration(style.click_effect_style);
    if (duration>0) for (size_t i=upper; i>0;) {
        const auto& e=events_[--i];
        if (e.time<time-duration) break;
        if (click_event(e)) {
            const double elapsed=std::max(time-e.time,0.0);
            sample.click_phase=PointerClickPhase{unit(elapsed/duration),elapsed,duration,e.kind};
            sample.is_clicking=true;
            break;
        }
    }
    if (motion.cursor!=CursorMotionStyle::kNone) {
        const bool smooth=motion.cursor==CursorMotionStyle::kSmooth;
        const bool medium=motion.cursor==CursorMotionStyle::kMedium;
        const double window=smooth ? 0.08 : (medium ? 0.065 : 0.05);
        const double maximum=smooth ? 9 : (medium ? 8 : 7);
        const double retained=smooth ? 0.38 : (medium ? 0.24 : 0.12);
        const double span=smooth ? 0.18 : (medium ? 0.22 : 0.28);
        const auto previous=resolved(time-window,motion), older=resolved(time-2*window,motion);
        if (previous && older) {
            const double vx=(sample.location.x-previous->x)/window*(1-retained)+(previous->x-older->x)/window*retained;
            const double vy=(sample.location.y-previous->y)/window*(1-retained)+(previous->y-older->y)/window*retained;
            const double speed=std::hypot(vx,vy), strength=std::clamp(style.motion_tilt_strength,2.0,4.0);
            const double threshold=0.006/strength;
            if (speed>threshold) {
                const double response=unit((speed-threshold)/(span/strength));
                const double torque=std::clamp((vx-vy)/speed/std::sqrt(2.0),-1.0,1.0);
                const double idle=unit(1-std::max(time-events_[lower].time,0.0)/0.18);
                sample.rotation_radians=-maximum*(0.35+0.65*strength)*response*response*(3-2*response)*torque*idle*std::numbers::pi/180;
            }
        }
    }
    out.cursor=sample;
    return out;
}
std::vector<PointerTrack::SpringSample> PointerTrack::build_spring(const MotionStyle& motion) const {
    if (events_.empty()) return {};
    const double interval = 1.0 / 240.0;
    const double mass = std::max(motion.cursor_spring_mass, 0.01);
    const double stiffness = std::max(motion.cursor_spring_stiffness, 0.01);
    const double damping = std::max(motion.cursor_spring_damping, 0.01);
    const double settle_duration = std::max(0.65, 6 * std::sqrt(mass / stiffness));
    const double end_time = std::max(events_.back().time, events_.front().time) + settle_duration;
    constexpr double jitter_threshold = 0.00035;
    constexpr double jitter_threshold_squared = jitter_threshold * jitter_threshold;
    NormalizedPoint position = events_.front().location;
    NormalizedPoint velocity{};
    NormalizedPoint stable_target = events_.front().location;
    double sample_time = events_.front().time;
    std::vector<SpringSample> result{{sample_time, position}};
    result.reserve(static_cast<size_t>((end_time - sample_time) / interval) + 2);
    while (sample_time < end_time) {
        const double next_time = std::min(sample_time + interval, end_time);
        const size_t event_upper = upper_index(next_time);
        const size_t lower_index = event_upper > 0 ? event_upper - 1 : 0;
        NormalizedPoint target;
        if (event_upper < events_.size()) {
            const auto& lower = events_[lower_index];
            const auto& upper = events_[event_upper];
            const double duration = std::max(upper.time - lower.time, std::numeric_limits<double>::min());
            if (continuous(lower, upper)) {
                const double progress = unit((next_time - lower.time) / duration);
                target = {lower.location.x + (upper.location.x - lower.location.x) * progress,
                          lower.location.y + (upper.location.y - lower.location.y) * progress};
            } else {
                target = lower.location;
            }
        } else {
            target = events_.back().location;
        }
        const double target_delta_x = target.x - stable_target.x;
        const double target_delta_y = target.y - stable_target.y;
        if (target_delta_x * target_delta_x + target_delta_y * target_delta_y >= jitter_threshold_squared)
            stable_target = target;
        const double delta_time = std::max(next_time - sample_time, 0.0);
        const double acceleration_x = (stiffness * (stable_target.x - position.x) - damping * velocity.x) / mass;
        const double acceleration_y = (stiffness * (stable_target.y - position.y) - damping * velocity.y) / mass;
        velocity.x += acceleration_x * delta_time;
        velocity.y += acceleration_y * delta_time;
        position = {unit(position.x + velocity.x * delta_time), unit(position.y + velocity.y * delta_time)};
        sample_time = next_time;
        result.push_back({sample_time, position});
    }

    std::vector<const PointerEventRecord*> clicks;
    for (const auto& event : events_) if (click_event(event)) clicks.push_back(&event);
    if (clicks.empty()) return result;
    std::vector<SpringSample> anchored;
    anchored.reserve(result.size() + clicks.size());
    size_t base_index = 0;
    for (const auto* click : clicks) {
        while (base_index < result.size() && result[base_index].time < click->time - 1e-7)
            anchored.push_back(result[base_index++]);
        if (base_index < result.size() && std::abs(result[base_index].time - click->time) <= 1e-7) {
            if (anchored.empty() || anchored.back().time != result[base_index].time)
                anchored.push_back(result[base_index]);
            ++base_index;
            continue;
        }
        if (anchored.empty() || base_index >= result.size()) continue;
        const auto& before = anchored.back();
        const auto& after = result[base_index];
        if (std::abs(before.time - click->time) <= 1e-7) continue;
        const double duration = std::max(after.time - before.time, std::numeric_limits<double>::min());
        const double progress = unit((click->time - before.time) / duration);
        anchored.push_back({click->time, blend(before.location, after.location, progress)});
    }
    anchored.insert(anchored.end(), result.begin() + static_cast<std::ptrdiff_t>(base_index), result.end());

    const auto first_index_at_or_after = [&](double time) {
        return static_cast<size_t>(std::lower_bound(anchored.begin(), anchored.end(), time,
            [](const auto& sample, double value) { return sample.time < value; }) - anchored.begin());
    };
    const auto first_index_after = [&](double time) {
        return static_cast<size_t>(std::upper_bound(anchored.begin(), anchored.end(), time,
            [](double value, const auto& sample) { return value < sample.time; }) - anchored.begin());
    };
    for (size_t click_index = 0; click_index < clicks.size(); ++click_index) {
        const auto& click = *clicks[click_index];
        const size_t anchor_index = first_index_at_or_after(click.time);
        if (anchor_index >= anchored.size() || std::abs(anchored[anchor_index].time - click.time) > 1e-7) continue;
        const std::optional<double> previous_click = click_index > 0
            ? std::optional<double>(clicks[click_index - 1]->time) : std::nullopt;
        const std::optional<double> next_click = click_index + 1 < clicks.size()
            ? std::optional<double>(clicks[click_index + 1]->time) : std::nullopt;
        const double start = std::max(anchored.front().time,
            std::max(click.time - 0.18, previous_click.value_or(-std::numeric_limits<double>::infinity())
                == -std::numeric_limits<double>::infinity() ? -std::numeric_limits<double>::infinity()
                    : (previous_click.value() + click.time) * 0.5));
        const double support_start = std::max(start, click.time - 0.18);
        const double support_end = std::min(anchored.back().time,
            std::min(click.time + 0.18, next_click.value_or(std::numeric_limits<double>::infinity())
                == std::numeric_limits<double>::infinity() ? std::numeric_limits<double>::infinity()
                    : (click.time + next_click.value()) * 0.5));
        const NormalizedPoint correction{click.location.x - anchored[anchor_index].location.x,
                                          click.location.y - anchored[anchor_index].location.y};
        for (size_t index = first_index_at_or_after(support_start);
             index < first_index_after(support_end); ++index) {
            const double sample_time_at = anchored[index].time;
            const double weight = sample_time_at <= click.time
                ? (click.time > support_start ? smoother((sample_time_at - support_start) /
                                                         (click.time - support_start)) : 1)
                : (support_end > click.time ? 1 - smoother((sample_time_at - click.time) /
                                                          (support_end - click.time)) : 0);
            anchored[index].location = {unit(anchored[index].location.x + correction.x * weight),
                                        unit(anchored[index].location.y + correction.y * weight)};
        }
        anchored[anchor_index].location = click.location;
    }
    return anchored;
}

ProjectPointerTrack::ProjectPointerTrack(PointerTrackSegments segments)
    : ProjectPointerTrack(std::move(segments), MotionStyle{}) {}
ProjectPointerTrack::ProjectPointerTrack(PointerTrackSegments segments,const MotionStyle& motion)
    : segment_records_(std::move(segments)) {
    std::stable_sort(segment_records_.begin(),segment_records_.end(),[](const auto& a,const auto& b){ return a.output_range.start<b.output_range.start; });
    for (const auto& record:segment_records_) {
        Segment s{record,PointerTrack(record.output_events,motion),{}};
        for (const auto& e:record.output_events) if (click_event(e) && e.time>=record.output_range.start && e.time<record.output_range.end()) { s.first_click_time=e.time; break; }
        segments_.push_back(std::move(s));
    }
    std::optional<NormalizedPoint> previous;
    double previous_end=-std::numeric_limits<double>::infinity();
    for (const auto& s : segments_) {
        if (s.record.output_range.start > previous_end + 1e-7) previous.reset();
        for (size_t index = 0; index < s.record.output_events.size(); ++index) {
            const auto& event = s.record.output_events[index];
            if (event.time < s.record.output_range.start || (s.record.starts_with_seed && index == 0)) continue;
            if (!previous || std::abs(previous->x - event.location.x) > 1e-6 ||
                std::abs(previous->y - event.location.y) > 1e-6 || event.kind != PointerEventKind::kMove)
                activity_times_.push_back(event.time);
            previous = event.location;
        }
        previous_end = s.record.output_range.end();
    }
}
ProjectPointerTrack ProjectPointerTrack::from_timeline(const TimelineMap& timeline,
    const std::vector<PointerEventRecord>& source_events, const MotionStyle& motion) {
    std::vector<PointerEventRecord> ordered = source_events;
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    std::vector<PointerTrackSegmentRecord> records;
    records.reserve(timeline.segments().size());
    for (size_t index = 0; index < timeline.segments().size(); ++index) {
        const auto& segment = timeline.segments()[index];
        const auto lower = std::lower_bound(ordered.begin(), ordered.end(), segment.source_start,
            [](const auto& event, double time) { return event.time < time; });
        const auto upper = std::lower_bound(lower, ordered.end(), segment.source_end(),
            [](const auto& event, double time) { return event.time < time; });
        std::vector<PointerEventRecord> source( lower, upper );
        std::vector<PointerEventRecord> output;
        output.reserve(source.size() + 1);
        const bool starts_exactly = !source.empty() && std::abs(source.front().time - segment.source_start) < 1e-6;
        const bool has_seed = !starts_exactly && lower != ordered.begin();
        if (has_seed) {
            const auto& seed = *(lower - 1);
            output.push_back({segment.output_start, seed.location, PointerEventKind::kMove,
                              seed.modifiers, seed.cursor_asset_id});
        }
        for (const auto& event : source) {
            output.push_back({segment.output_start + (event.time - segment.source_start) / segment.playback_rate,
                              event.location, event.kind, event.modifiers, event.cursor_asset_id});
        }
        const double previous_source_end = index > 0 ? timeline.segments()[index - 1].source_end() : 0;
        const bool discontinuity = index > 0 && std::abs(segment.source_start - previous_source_end) > 1e-4;
        records.push_back({segment.id, segment.output_range(), std::move(source), std::move(output),
                           has_seed, discontinuity});
    }
    return ProjectPointerTrack(std::move(records), motion);
}
std::optional<size_t> ProjectPointerTrack::segment_index(double time) const {
    if (!std::isfinite(time)) return {};
    auto it=std::upper_bound(segments_.begin(),segments_.end(),time,
        [](double t,const Segment& s){return t<s.record.output_range.start;});
    if (it==segments_.begin()) return {};
    --it;
    if (time<it->record.output_range.start || time>=it->record.output_range.end()) return {};
    return static_cast<size_t>(it-segments_.begin());
}
std::optional<NormalizedPoint> ProjectPointerTrack::raw_position(double time,size_t index) const {
    if (index>=segments_.size()) return {};
    auto result=raw_interpolate(segments_[index].record.output_events,time);
    return result ? std::optional<NormalizedPoint>(result->location) : std::nullopt;
}
std::optional<NormalizedPoint> ProjectPointerTrack::position(double time,const MotionStyle& motion) const {
    const auto index=segment_index(time);
    if (!index) return {};
    return segments_[*index].track.position(time,motion);
}
bool ProjectPointerTrack::automatic_entry_starts_after_cut(const ZoomClip& clip) const {
    if (clip.origin != ZoomKeyframeOrigin::kAutomatic) return false;
    const auto index = segment_index(clip.start_time);
    return index && segments_[*index].record.starts_after_source_discontinuity &&
        std::abs(segments_[*index].record.output_range.start - clip.start_time) <= 1e-4;
}
PointerTrackEvaluation ProjectPointerTrack::evaluation(double time, const MotionStyle& motion,
    const CursorStyle& style, const ZoomClip* automatic_entry) const {
    const auto index = segment_index(time);
    if (!index) return {};
    const auto& segment = segments_[*index];
    CursorStyle segment_style = style;
    segment_style.hide_when_idle = false;
    auto current = segment.track.evaluation(time, motion, segment_style);
    if (style.hide_when_idle) {
        const auto activity = std::upper_bound(activity_times_.begin(), activity_times_.end(), time);
        if (activity == activity_times_.begin() || time - *(activity - 1) > style.idle_delay)
            current.cursor.reset();
    }
    if (automatic_entry && automatic_entry_starts_after_cut(*automatic_entry) &&
        std::abs(automatic_entry->start_time - segment.record.output_range.start) <= 1e-4)
        return current;
    if (*index == 0) return current;
    const double start = segment.record.output_range.start;
    const double until_click = segment.first_click_time ? std::max(*segment.first_click_time - start, 0.0) : 0.28;
    const double duration = std::min({0.28, until_click, segment.record.output_range.duration});
    if (duration <= 1e-7 || time < start || time >= start + duration) return current;
    const auto& previous = segments_[*index - 1];
    const double previous_time = std::max(previous.record.output_range.start,
                                          previous.record.output_range.end() - 1e-4);
    const auto from = previous.track.position(previous_time, motion);
    const auto to = segment.track.position(start + duration, motion);
    if (!from || !to) return current;
    const NormalizedPoint bridge = blend(*from, *to, smoother((time - start) / duration));
    if (current.cursor) current.cursor->location = bridge;
    current.position = bridge;
    return current;
}
std::vector<ProjectPointerTrack::FocusSample> ProjectPointerTrack::build_automatic_focus(
    const ZoomClip& clip, const MotionStyle& motion, bool reanchor_entry) const {
    std::vector<FocusSample> samples;
    if (clip.origin != ZoomKeyframeOrigin::kAutomatic || !std::isfinite(clip.start_time) ||
        !std::isfinite(clip.end_time) || clip.end_time <= clip.start_time) return samples;
    NormalizedPoint position = clip.focus, target = clip.focus, velocity{};
    const auto start_segment = segment_index(clip.start_time);
    if (start_segment && !reanchor_entry && clip.enter_progress_offset <= 1e-4) {
        const auto& segment = segments_[*start_segment];
        const double entry_end = std::min(clip.start_time + std::min(clip.enter_duration, clip.duration()),
                                          segment.record.output_range.end());
        const auto click = std::find_if(segment.record.output_events.begin(), segment.record.output_events.end(),
            [&](const auto& event) { return event.time >= clip.start_time && event.time <= entry_end && click_event(event); });
        if (click != segment.record.output_events.end()) position = target = click->location;
        else if (const auto raw = raw_position(clip.start_time, *start_segment)) position = target = *raw;
    }
    const double half = std::max(1.0 / (2 * std::max(clip.scale, 1.0)) * 0.56, 0.01);
    const double follow = std::min(clip.start_time + std::min(clip.enter_duration, clip.duration()), clip.end_time);
    const double mass = std::max(motion.screen_spring_mass, 0.01);
    const double stiffness = std::max(motion.screen_spring_stiffness, 0.01);
    const double damping = std::max(motion.screen_spring_damping, 0.01);
    double time = clip.start_time;
    samples.push_back({time, position});
    while (time < clip.end_time) {
        const double next = std::min(time + 1.0 / 120.0, clip.end_time);
        const double dt = next - time;
        if (next >= follow) {
            if (const auto segment = segment_index(next)) {
                if (const auto raw = raw_position(next, *segment)) {
                    if (raw->x < target.x - half || raw->x > target.x + half) target.x = raw->x;
                    if (raw->y < target.y - half || raw->y > target.y + half) target.y = raw->y;
                }
            }
        }
        velocity.x += (stiffness * (target.x - position.x) - damping * velocity.x) / mass * dt;
        velocity.y += (stiffness * (target.y - position.y) - damping * velocity.y) / mass * dt;
        position = {unit(position.x + velocity.x * dt), unit(position.y + velocity.y * dt)};
        time = next;
        samples.push_back({time, position});
    }
    return samples;
}

NormalizedPoint ProjectPointerTrack::automatic_camera_focus(const ZoomClip& clip,double time,
    const MotionStyle& motion,bool reanchor_entry) const {
    if (clip.origin != ZoomKeyframeOrigin::kAutomatic || !std::isfinite(time) ||
        !std::isfinite(clip.end_time) || clip.end_time <= clip.start_time) return clip.focus;
    const FocusKey key{clip.id, clip.start_time, clip.end_time, clip.scale, clip.focus,
        clip.enter_duration, clip.enter_progress_offset,
        std::max(motion.screen_spring_mass, 0.01),
        std::max(motion.screen_spring_stiffness, 0.01),
        std::max(motion.screen_spring_damping, 0.01), reanchor_entry};
    std::lock_guard lock(focus_cache_->mutex);
    ++focus_cache_->clock;
    auto cached = std::find_if(focus_cache_->variants.begin(), focus_cache_->variants.end(),
        [&](const auto& variant) { return variant.key == key; });
    if (cached == focus_cache_->variants.end()) {
        auto samples = build_automatic_focus(clip, motion, reanchor_entry);
        if (focus_cache_->variants.size() == 4) {
            const auto oldest = std::min_element(focus_cache_->variants.begin(), focus_cache_->variants.end(),
                [](const auto& a, const auto& b) { return a.last_used < b.last_used; });
            focus_cache_->variants.erase(oldest);
        }
        focus_cache_->variants.push_back({key, focus_cache_->clock, std::move(samples)});
        cached = std::prev(focus_cache_->variants.end());
    } else {
        cached->last_used = focus_cache_->clock;
    }
    const auto& samples = cached->samples;
    if (samples.empty()) return clip.focus;
    const double clamped = std::clamp(time, clip.start_time, clip.end_time);
    const auto upper = std::lower_bound(samples.begin(), samples.end(), clamped,
        [](const auto& sample, double value) { return sample.time < value; });
    if (upper == samples.begin()) return upper->position;
    if (upper == samples.end()) return samples.back().position;
    if (upper->time == clamped) return upper->position;
    const auto& lower = *(upper - 1);
    const double span = upper->time - lower.time;
    return span > 0 ? blend(lower.position, upper->position, (clamped - lower.time) / span)
                    : lower.position;
}

}  // namespace dogsc
