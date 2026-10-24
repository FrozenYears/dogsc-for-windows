#include "dogsc/project_timeline_editing.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <type_traits>
#include <set>

namespace dogsc {
namespace {
using E = ProjectTimelineEditingError;
using C = ProjectTimelineEditingErrorCode;
using T = ProjectTimelineTrack;
using R = ProjectTimelineEditResult;
E err(C code, Uuid id = {}, Uuid related = {}, T track = T::kPrimaryRecording) { return {code, track, id, related}; }
bool ok(double x) { return std::isfinite(x); }
bool point(NormalizedPoint p) { return ok(p.x) && ok(p.y) && p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1; }
void zoom_sort(std::vector<ZoomClip>& v) { std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.start_time == b.start_time ? a.id.to_string() < b.id.to_string() : a.start_time < b.start_time; }); }
void screen_sort(std::vector<ScreenMotionClip>& v) { std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.timing.start_time == b.timing.start_time ? a.id.to_string() < b.id.to_string() : a.timing.start_time < b.timing.start_time; }); }
void camera_sort(std::vector<CameraMotionClip>& v) { std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.timing.start_time == b.timing.start_time ? a.id.to_string() < b.id.to_string() : a.timing.start_time < b.timing.start_time; }); }
SourceSequence edit(std::vector<RecordingSegment> v) { SourceSequence s; s.is_full_recording = false; s.segments = std::move(v); return s; }
std::vector<RecordingSegment> segments(const RecorderProject& p, const TimelineMap& m) { return p.source_sequence.is_full_recording ? m.materialized_source_sequence().segments : p.source_sequence.segments; }
void insert(RecorderProject& p, double at, double d) { for (auto& c : p.zoom_clips) { if (c.start_time >= at - ProjectTimelineEditing::kEpsilon) { c.start_time += d; c.end_time += d; } else if (c.end_time > at + ProjectTimelineEditing::kEpsilon) c.end_time += d; } auto apply=[&]<class X>(std::vector<X>& v) { for (auto& c:v) { if (c.timing.start_time >= at-ProjectTimelineEditing::kEpsilon) c.timing.start_time += d; else if(c.timing.end_time()>at+ProjectTimelineEditing::kEpsilon) c.timing.duration += d; } }; apply(p.screen_motion_clips);apply(p.camera_motion_clips);apply(p.mosaic_clips);apply(p.sticker_clips);zoom_sort(p.zoom_clips);screen_sort(p.screen_motion_clips);camera_sort(p.camera_motion_clips); }
constexpr double adjacency = 1.0 / 120.0;
constexpr double epsilon = ProjectTimelineEditing::kEpsilon;
double mapped(double t,double a,double b) { return t<=a?t:t>=b?t-(b-a):a; }
void preserve(ZoomClip& c) {
    if(!c.preferred_enter_duration)c.preferred_enter_duration=c.enter_duration;
    if(!c.preferred_exit_duration)c.preferred_exit_duration=c.exit_duration>0.0001?c.exit_duration:0.7;
}
double requested_exit(const ZoomClip& c,double def) { return c.preferred_exit_duration.value_or(c.exit_duration>0.0001?c.exit_duration:std::max(def,0.0)); }
double continued(double offset,double start,double duration,double a,double b) {
    offset=std::clamp(offset,0.0,1.0);
    if(duration<=epsilon||start<a-epsilon||start>=b-epsilon)return offset;
    return offset+(1-offset)*std::clamp((a-start)/duration,0.0,1.0);
}
template<class X> void ripple(std::vector<X>& v,double a,double b,bool tail) {
    std::vector<X> r;r.reserve(v.size());
    for(auto c:v){auto old=c.timing;
        if constexpr(std::is_same_v<decltype(old),TransitionTiming>) {
            c.timing.preserve_transition_intent();old=c.timing;
            if(old.effect_end_time()<=a+epsilon){r.push_back(std::move(c));continue;}
            if(old.start_time>=a-epsilon&&old.end_time()<=b+epsilon)continue;
            if(tail){if(old.start_time<a-epsilon)r.push_back(std::move(c));continue;}
        } else {
            if(old.end_time()<=a+epsilon){r.push_back(std::move(c));continue;}
            if(tail){if(old.start_time<a-epsilon){c.timing.duration=std::max(a-old.start_time,0.0);r.push_back(std::move(c));}continue;}
        }
        if(old.start_time>=b-epsilon){c.timing.start_time-=b-a;r.push_back(std::move(c));continue;}
        if(old.start_time>=a-epsilon&&old.end_time()<=b+epsilon)continue;
        const double start=mapped(old.start_time,a,b),end=mapped(old.end_time(),a,b);
        if(end-start<=epsilon)continue;
        c.timing.start_time=start;c.timing.duration=end-start;
        if constexpr(std::is_same_v<decltype(old),TransitionTiming>) {
            c.timing.lead_in_duration=std::clamp(mapped(old.lead_in_end_time(),a,b)-start,0.0,end-start);
            c.timing.return_duration=std::max(mapped(old.effect_end_time(),a,b)-end,0.0);
            c.timing.lead_in_progress_offset=continued(old.lead_in_progress_offset,old.start_time,std::min(old.lead_in_duration,old.duration),a,b);
            c.timing.return_progress_offset=continued(old.return_progress_offset,old.end_time(),old.return_duration,a,b);
        }
        r.push_back(std::move(c));
    }v=std::move(r);
}
void ripple_zoom(std::vector<ZoomClip>& v,double a,double b,bool tail) {
    std::vector<ZoomClip> r;r.reserve(v.size());
    for(auto c:v){preserve(c);const auto old=c;
        if(c.effect_end_time()<=a+epsilon){r.push_back(std::move(c));continue;}
        if(tail){if(c.start_time<a-epsilon)r.push_back(std::move(c));continue;}
        if(c.start_time>=b-epsilon){c.start_time-=b-a;c.end_time-=b-a;r.push_back(std::move(c));continue;}
        if(c.start_time>=a-epsilon&&c.end_time<=b+epsilon)continue;
        c.start_time=mapped(old.start_time,a,b);c.end_time=mapped(old.end_time,a,b);
        if(c.duration()<=epsilon)continue;
        c.enter_duration=std::clamp(mapped(std::min(old.start_time+old.enter_duration,old.end_time),a,b)-c.start_time,0.0,c.duration());
        c.exit_duration=std::max(mapped(old.effect_end_time(),a,b)-c.end_time,0.0);
        c.enter_progress_offset=continued(old.enter_progress_offset,old.start_time,std::min(old.enter_duration,old.duration()),a,b);
        c.exit_progress_offset=continued(old.exit_progress_offset,old.end_time,old.exit_duration,a,b);
        r.push_back(std::move(c));
    }v=std::move(r);
}
template<class X> void restore_phases(const std::vector<X>& original,std::vector<X>& clips,double a,double b,double def) {
    for(size_t i=0;i<clips.size();++i){auto& c=clips[i];auto it=std::find_if(original.begin(),original.end(),[&](const auto& x){return x.id==c.id;});if(it==original.end())continue;
        if constexpr(std::is_same_v<X,ZoomClip>) {
            const auto& old=*it;
            if(std::min(old.enter_duration,old.duration())>epsilon&&old.start_time<b-epsilon&&old.start_time+std::min(old.enter_duration,old.duration())>a+epsilon){c.enter_duration=std::min(old.preferred_enter_duration.value_or(old.enter_duration),c.duration());c.enter_progress_offset=old.start_time>=a-epsilon?0:old.enter_progress_offset;}
            const double requested=requested_exit(old,def),gap=i+1<clips.size()?std::max(clips[i+1].start_time-c.end_time,0.0):requested;
            c.preferred_exit_duration=requested;
            if(i+1<clips.size()&&gap<=adjacency)continue;
            c.exit_duration=std::min(requested,gap);c.exit_progress_offset=old.end_time>=a-epsilon&&old.end_time<b-epsilon?0:old.exit_progress_offset;
            if(old.exit_duration<=epsilon)c.exit_progress_offset=0;
        } else {
            const auto& old=it->timing;auto& t=c.timing;
            if(std::min(old.lead_in_duration,old.duration)>epsilon&&old.start_time<b-epsilon&&old.start_time+std::min(old.lead_in_duration,old.duration)>a+epsilon){t.lead_in_duration=std::min(old.requested_lead_in_duration(),t.duration);t.lead_in_progress_offset=old.start_time>=a-epsilon?0:old.lead_in_progress_offset;}
            const X* successor=nullptr;for(const auto& x:original)if(x.timing.start_time>old.start_time||(x.timing.start_time==old.start_time&&x.id.to_string()>it->id.to_string()))if(!successor||x.timing.start_time<successor->timing.start_time||(x.timing.start_time==successor->timing.start_time&&x.id.to_string()<successor->id.to_string()))successor=&x;
            const bool touched=successor&&std::abs(successor->timing.start_time-old.end_time())<=adjacency;
            if(old.return_duration<=epsilon&&!old.preferred_return_duration&&!touched){t.preferred_return_duration=0;continue;}
            const double requested=old.requested_return_duration(def),gap=i+1<clips.size()?std::max(clips[i+1].timing.start_time-t.end_time(),0.0):requested;
            t.preferred_return_duration=requested;if(i+1<clips.size()&&gap<=adjacency)continue;
            t.return_duration=std::min(requested,gap);t.return_progress_offset=old.end_time()>=a-epsilon&&old.end_time()<b-epsilon?0:old.return_progress_offset;
            if(old.return_duration<=epsilon)t.return_progress_offset=0;
        }
    }
}
R ripple_project(const RecorderProject&p,SourceSequence s,double a,double b,bool tail,double full,double def){RecorderProject o=p;o.source_sequence=std::move(s);std::set<Uuid,UuidLess> valid;for(auto&x:o.source_sequence.segments)valid.insert(x.id);for(auto it=o.primary_segment_audio_overrides.begin();it!=o.primary_segment_audio_overrides.end();)if(!valid.contains(it->first)||it->second.is_empty())it=o.primary_segment_audio_overrides.erase(it);else ++it;ripple_zoom(o.zoom_clips,a,b,tail);ripple(o.screen_motion_clips,a,b,tail);ripple(o.camera_motion_clips,a,b,tail);ripple(o.mosaic_clips,a,b,tail);ripple(o.sticker_clips,a,b,tail);zoom_sort(o.zoom_clips);screen_sort(o.screen_motion_clips);camera_sort(o.camera_motion_clips);if(!tail){restore_phases(p.zoom_clips,o.zoom_clips,a,b,def);restore_phases(p.screen_motion_clips,o.screen_motion_clips,a,b,def);restore_phases(p.camera_motion_clips,o.camera_motion_clips,a,b,def);}if(auto e=ProjectTimelineEditing::validate(o,full))return *e;return o;}
template<class X> void normalize(std::vector<X>& clips) {
    const auto start=[](const X& c){if constexpr(std::is_same_v<X,ZoomClip>)return c.start_time;else return c.timing.start_time;};
    std::sort(clips.begin(),clips.end(),[&](const X& a,const X& b){return start(a)==start(b)?a.id.to_string()<b.id.to_string():start(a)<start(b);});
    size_t count=0;
    for(size_t i=0;i<clips.size();++i){
        if(count){auto& previous=clips[count-1];const double next=start(clips[i]);
            if constexpr(std::is_same_v<X,ZoomClip>){
                if(previous.end_time>next){previous.end_time=std::max(next,previous.start_time);previous.enter_duration=std::min(previous.enter_duration,previous.duration());}
                if(previous.duration()<=epsilon)--count;else if(next-previous.end_time>adjacency)previous.exit_duration=std::min(previous.exit_duration,next-previous.end_time);
            }else{
                auto& t=previous.timing;if(t.end_time()>next){t.duration=std::max(next-t.start_time,0.0);t.lead_in_duration=std::min(t.lead_in_duration,t.duration);}
                if(t.duration<=epsilon)--count;else if(next-t.end_time()>adjacency)t.return_duration=std::min(t.return_duration,next-t.end_time());
            }
        }
        if(count!=i)clips[count]=std::move(clips[i]);++count;
    }clips.resize(count);
}
}

PrimarySegmentAudioOverrides ProjectTimelineEditing::primary_segment_audio_overrides(const RecorderProject&p,const Uuid&id){auto it=p.primary_segment_audio_overrides.find(id);return it==p.primary_segment_audio_overrides.end()?PrimarySegmentAudioOverrides{}:it->second;}
std::optional<E> ProjectTimelineEditing::validate(const RecorderProject&p){
    const auto range=[](double x,double lo,double hi){return ok(x)&&x>=lo&&x<=hi;};
    const auto optional_duration=[&](const std::optional<double>& x){return !x||range(*x,0,5);};
    const auto curve=[](const ZoomBezierCurve& c){return ok(c.x1)&&ok(c.y1)&&ok(c.x2)&&ok(c.y2);};
    if(!p.source_sequence.is_full_recording){
        if(p.source_sequence.segments.empty())return err(C::kInvalidSourceSequence);
        std::set<Uuid,UuidLess> ids;
        for(const auto&s:p.source_sequence.segments){
            if(!ids.insert(s.id).second)return err(C::kDuplicateSegmentID,s.id);
            if(!ok(s.source_start)||!ok(s.source_duration)||!ok(s.playback_rate)||!ok(s.source_end())||s.source_start<0||s.source_duration<=0||s.playback_rate<=0)return err(C::kInvalidSourceSequence);
            for(const auto&o:p.source_sequence.segments)if(s.id!=o.id&&s.source_start<o.source_end()-kEpsilon&&s.source_end()>o.source_start+kEpsilon)return err(C::kInvalidSourceSequence);
        }
    }
    std::set<Uuid,UuidLess> primary;
    if(p.source_sequence.is_full_recording)primary.insert(TimelineMap::full_recording_segment_id());else for(const auto&s:p.source_sequence.segments)primary.insert(s.id);
    for(const auto&[id,o]:p.primary_segment_audio_overrides)if(!primary.contains(id)||o.is_empty()||(o.system_volume&&!range(*o.system_volume,0,1))||(o.microphone_volume&&!range(*o.microphone_volume,0,1)))return err(C::kInvalidPrimarySegmentAudioOverrides,id);
    // 按起点排序引用，不复制整段效果或贴图路径。
    auto check=[&]<class X>(const std::vector<X>& clips,T track)->std::optional<E>{
        std::vector<const X*> ordered;ordered.reserve(clips.size());for(const auto& c:clips)ordered.push_back(&c);
        const auto start=[](const X& c){if constexpr(std::is_same_v<X,ZoomClip>)return c.start_time;else return c.timing.start_time;};
        std::sort(ordered.begin(),ordered.end(),[&](const X* a,const X* b){return start(*a)==start(*b)?a->id.to_string()<b->id.to_string():start(*a)<start(*b);});
        std::set<Uuid,UuidLess> ids;const X* previous=nullptr;
        for(const X* ptr:ordered){const auto& c=*ptr;if(!ids.insert(c.id).second)return err(C::kDuplicateClipID,c.id,{},track);bool valid=true;
            if constexpr(std::is_same_v<X,ZoomClip>){
                valid=ok(c.start_time)&&ok(c.end_time)&&c.start_time>=0&&c.end_time>=c.start_time&&ok(c.scale)&&c.scale>=1&&point(c.focus)&&range(c.enter_duration,0,5)&&range(c.exit_duration,0,5)&&optional_duration(c.preferred_enter_duration)&&optional_duration(c.preferred_exit_duration)&&range(c.enter_progress_offset,0,1)&&range(c.exit_progress_offset,0,1)&&curve(c.custom_curve)&&(!c.focus_effect||c.focus_effect->is_valid());
            }else{
                const auto&t=c.timing;valid=ok(t.start_time)&&t.start_time>=0&&ok(t.duration)&&t.duration>0&&ok(t.end_time());
                if constexpr(std::is_same_v<X,ScreenMotionClip>||std::is_same_v<X,CameraMotionClip>){
                    valid=valid&&range(t.lead_in_duration,0,5)&&range(t.return_duration,0,5)&&range(t.lead_in_progress_offset,0,1)&&range(t.return_progress_offset,0,1)&&optional_duration(t.preferred_lead_in_duration)&&optional_duration(t.preferred_return_duration)&&curve(t.custom_curve)&&point(c.target.position);
                    if constexpr(std::is_same_v<X,ScreenMotionClip>)valid=valid&&ok(c.target.scale)&&c.target.scale>0&&ok(c.target.rotation_x)&&ok(c.target.rotation_y)&&ok(c.target.rotation_z)&&ok(c.target.perspective)&&c.target.perspective>=0&&(!c.focus_effect||c.focus_effect->is_valid());
                    else valid=valid&&ok(c.target.size)&&c.target.size>0&&range(c.target.roundness,0,1)&&range(c.target.opacity,0,1);
                }else if constexpr(std::is_same_v<X,MosaicClip>){
                    valid=valid&&c.source_rect==c.source_rect.clamped()&&range(c.corner_radius,0,0.5)&&range(c.intensity,0,1)&&range(c.spotlight_dimming,0,0.75)&&range(c.transition_in_duration,0,5)&&range(c.transition_out_duration,0,5);
                }else{
                    bool path_valid=!c.relative_path.empty()&&c.relative_path.front()!='/';
                    for(size_t at=0;at<c.relative_path.size();){const auto end=c.relative_path.find('/',at);if(c.relative_path.substr(at,end==std::string::npos?end:end-at)=="..")path_valid=false;if(end==std::string::npos)break;at=end+1;}
                    valid=valid&&path_valid&&point(c.position)&&range(c.width,0.02,1.5)&&range(c.rotation_degrees,-1080,1080)&&range(c.opacity,0,1)&&range(c.corner_radius,0,500)&&range(c.border_width,0,60)&&range(c.shadow_opacity,0,1)&&range(c.shadow_radius,0,160)&&ok(c.shadow_offset_x)&&ok(c.shadow_offset_y)&&range(c.enter_duration,0,5)&&range(c.exit_duration,0,5)&&range(c.backdrop_blur,0,96);
                }
            }
            if(!valid)return err(C::kInvalidClip,c.id,{},track);
            if constexpr(std::is_same_v<X,ZoomClip>){if(previous&&(c.start_time<previous->end_time-0.0001||(c.start_time-previous->end_time>adjacency&&c.start_time<previous->effect_end_time()-0.0001)))return err(C::kOverlappingClips,previous->id,c.id,track);}
            else if constexpr(std::is_same_v<X,ScreenMotionClip>||std::is_same_v<X,CameraMotionClip>){if(previous&&(c.timing.start_time<previous->timing.end_time()-kEpsilon||(c.timing.start_time-previous->timing.end_time()>adjacency&&c.timing.start_time<previous->timing.effect_end_time()-kEpsilon)))return err(C::kOverlappingClips,previous->id,c.id,track);}
            previous=ptr;
        }return {};
    };
    if(auto e=check(p.zoom_clips,T::kZoom))return e;if(auto e=check(p.screen_motion_clips,T::kScreenMotion))return e;if(auto e=check(p.camera_motion_clips,T::kCameraMotion))return e;if(auto e=check(p.mosaic_clips,T::kMosaic))return e;return check(p.sticker_clips,T::kSticker);
}
std::optional<E> ProjectTimelineEditing::validate(const RecorderProject&p,double d){TimelineMap m;if(TimelineMap::create(p.source_sequence,d,m))return err(C::kInvalidSourceSequence);return validate(p);}
R ProjectTimelineEditing::setting_primary_segment_audio_overrides(const RecorderProject&p,const Uuid&id,const PrimarySegmentAudioOverrides&o){bool found=p.source_sequence.is_full_recording?id==TimelineMap::full_recording_segment_id():std::any_of(p.source_sequence.segments.begin(),p.source_sequence.segments.end(),[&](auto&s){return s.id==id;});if(!found)return err(C::kSegmentNotFound,id);auto q=p;if(o.is_empty())q.primary_segment_audio_overrides.erase(id);else q.primary_segment_audio_overrides[id]=o;if(auto e=validate(q))return *e;return q;}
R ProjectTimelineEditing::materialized(const RecorderProject&p,double d){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);auto q=p;q.source_sequence=m.materialized_source_sequence();return q;}
R ProjectTimelineEditing::split_primary_segment(const RecorderProject&p,double t,const Uuid&right,double d){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);auto*r=m.segment_at_output_time(t);if(!ok(t)||!r||t<=r->output_start+kEpsilon||t>=r->output_end()-kEpsilon)return err(C::kSplitOutsideSegment);for(auto&s:m.segments())if(s.id==right)return err(C::kDuplicateSegmentID,right);auto v=segments(p,m);auto i=std::find_if(v.begin(),v.end(),[&](auto&s){return s.id==r->id;});double x=(t-r->output_start)*r->playback_rate;i->source_duration=x;v.insert(i+1,{right,r->source_start+x,r->source_duration-x,r->playback_rate});auto q=p;q.source_sequence=edit(std::move(v));if(auto o=p.primary_segment_audio_overrides.find(r->id);o!=p.primary_segment_audio_overrides.end())q.primary_segment_audio_overrides[right]=o->second;return q;}
R ProjectTimelineEditing::trim_primary_segment(const RecorderProject&p,const Uuid&id,RecordingSegmentTrimEdge edge,double t,double d,TimeInterval def){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);auto i=std::find_if(m.segments().begin(),m.segments().end(),[&](auto&s){return s.id==id;});if(i==m.segments().end())return err(C::kSegmentNotFound,id);auto&r=*i;if(!ok(t))return err(C::kTrimOutsideSegment,id);auto v=segments(p,m);auto n=static_cast<size_t>(i-m.segments().begin());if(edge==RecordingSegmentTrimEdge::kLeft&&std::abs(t-r.output_start)<=kEpsilon)return p;if(edge==RecordingSegmentTrimEdge::kRight&&std::abs(t-r.output_end())<=kEpsilon)return p;if(edge==RecordingSegmentTrimEdge::kLeft&&t<r.output_start-kEpsilon){double source=(r.output_start-t)*r.playback_rate,prior=0;for(size_t j=0;j<v.size();++j)if(j!=n&&v[j].source_end()<=r.source_start+kEpsilon)prior=std::max(prior,v[j].source_end());if(source>r.source_start-prior+kEpsilon)return err(C::kTrimOutsideSegment,id);v[n].source_start-=source;v[n].source_duration+=source;auto q=p;q.source_sequence=edit(std::move(v));insert(q,r.output_start,source/r.playback_rate);if(auto e=validate(q,d))return *e;return q;}if(edge==RecordingSegmentTrimEdge::kRight&&t>r.output_end()+kEpsilon){double source=(t-r.output_end())*r.playback_rate,next=d;for(size_t j=0;j<v.size();++j)if(j!=n&&v[j].source_start>=r.source_end()-kEpsilon)next=std::min(next,v[j].source_start);if(source>next-r.source_end()+kEpsilon)return err(C::kTrimOutsideSegment,id);v[n].source_duration+=source;auto q=p;q.source_sequence=edit(std::move(v));insert(q,r.output_end(),source/r.playback_rate);if(auto e=validate(q,d))return *e;return q;}if(t<=r.output_start+kEpsilon||t>=r.output_end()-kEpsilon)return err(C::kTrimOutsideSegment,id);double a=edge==RecordingSegmentTrimEdge::kLeft?r.output_start:t,b=edge==RecordingSegmentTrimEdge::kLeft?t:r.output_end();if(edge==RecordingSegmentTrimEdge::kLeft){double source=(t-r.output_start)*r.playback_rate;v[n].source_start+=source;v[n].source_duration-=source;}else v[n].source_duration=(t-r.output_start)*r.playback_rate;return ripple_project(p,edit(std::move(v)),a,b,b>=m.output_duration()-kEpsilon,d,def);}
R ProjectTimelineEditing::remove_primary_segment(const RecorderProject&p,const Uuid&id,double d,TimeInterval def){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);auto i=std::find_if(m.segments().begin(),m.segments().end(),[&](auto&s){return s.id==id;});if(i==m.segments().end())return err(C::kSegmentNotFound,id);if(m.segments().size()==1)return err(C::kCannotRemoveFinalSegment);auto v=segments(p,m);v.erase(v.begin()+(i-m.segments().begin()));return ripple_project(p,edit(std::move(v)),i->output_start,i->output_end(),i->output_end()>=m.output_duration()-kEpsilon,d,def);}
R ProjectTimelineEditing::move_primary_segment(const RecorderProject&p,const Uuid&id,int to,double d){if(auto e=validate(p,d))return *e;TimelineMap old;TimelineMap::create(p.source_sequence,d,old);auto i=std::find_if(old.segments().begin(),old.segments().end(),[&](auto&s){return s.id==id;});if(i==old.segments().end())return err(C::kSegmentNotFound,id);auto v=segments(p,old);auto from=static_cast<size_t>(i-old.segments().begin());auto target=static_cast<size_t>(std::clamp(to,0,static_cast<int>(v.size()-1)));if(from==target)return p;auto moved=v[from];v.erase(v.begin()+from);v.insert(v.begin()+target,moved);TimelineMap fresh;TimelineMap::create(edit(v),d,fresh);std::map<Uuid,double,UuidLess> starts;for(auto&s:fresh.segments())starts[s.id]=s.output_start;auto delta=[&](double time){auto*owner=old.segment_at_output_time(std::clamp(time,0.0,std::max(old.output_duration()-kEpsilon,0.0)));return owner?starts[owner->id]-owner->output_start:0.0;};auto q=p;q.source_sequence=edit(std::move(v));for(auto&c:q.zoom_clips){preserve(c);double shift=delta(c.start_time);c.start_time=std::clamp(c.start_time+shift,0.0,fresh.output_duration());c.end_time=std::clamp(c.end_time+shift,c.start_time,fresh.output_duration());c.enter_duration=std::min(c.enter_duration,c.duration());c.exit_duration=std::min(c.exit_duration,std::max(fresh.output_duration()-c.end_time,0.0));}auto remap=[&]<class X>(std::vector<X>&clips){std::vector<X> kept;for(auto c:clips){c.timing.start_time=std::clamp(c.timing.start_time+delta(c.timing.start_time),0.0,fresh.output_duration());c.timing.duration=std::min(c.timing.duration,std::max(fresh.output_duration()-c.timing.start_time,0.0));if constexpr(!std::is_same_v<X,MosaicClip>&&!std::is_same_v<X,StickerClip>){c.timing.lead_in_duration=std::min(c.timing.lead_in_duration,c.timing.duration);c.timing.return_duration=std::min(c.timing.return_duration,std::max(fresh.output_duration()-c.timing.end_time(),0.0));}if(c.timing.duration>kEpsilon)kept.push_back(c);}clips=std::move(kept);};remap(q.screen_motion_clips);remap(q.camera_motion_clips);remap(q.mosaic_clips);remap(q.sticker_clips);q.zoom_clips.erase(std::remove_if(q.zoom_clips.begin(),q.zoom_clips.end(),[](const auto& c){return c.duration()<=kEpsilon;}),q.zoom_clips.end());normalize(q.zoom_clips);normalize(q.screen_motion_clips);normalize(q.camera_motion_clips);std::sort(q.mosaic_clips.begin(),q.mosaic_clips.end(),[](const auto& a,const auto& b){return a.timing.start_time==b.timing.start_time?a.id.to_string()<b.id.to_string():a.timing.start_time<b.timing.start_time;});std::sort(q.sticker_clips.begin(),q.sticker_clips.end(),[](const auto& a,const auto& b){return a.layer_index==b.layer_index?a.id.to_string()<b.id.to_string():a.layer_index<b.layer_index;});if(auto e=validate(q,d))return *e;return q;}
R ProjectTimelineEditing::restore_primary_gap(const RecorderProject&p,const Uuid&a,const Uuid&b,const Uuid&id,double d){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);for(auto&s:m.segments())if(s.id==id)return err(C::kDuplicateSegmentID,id);auto i=std::find_if(m.segments().begin(),m.segments().end(),[&](auto&s){return s.id==a;});if(i==m.segments().end()||i+1==m.segments().end()||(i+1)->id!=b)return err(C::kRestoreJunctionNotFound,a,b);double gap=(i+1)->source_start-i->source_end();if(gap<=kEpsilon)return err(C::kNoRestorableSourceGap,a,b);auto v=segments(p,m);v.insert(v.begin()+(i-m.segments().begin())+1,{id,i->source_end(),gap,1});auto q=p;q.source_sequence=edit(std::move(v));if(auto o=p.primary_segment_audio_overrides.find(a);o!=p.primary_segment_audio_overrides.end())q.primary_segment_audio_overrides[id]=o->second;insert(q,i->output_end(),gap);if(auto e=validate(q,d))return *e;return q;}
R ProjectTimelineEditing::restore_primary_leading_gap(const RecorderProject&p,const Uuid&id,double d){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);if(m.segments().empty()||m.segments().front().source_start<=kEpsilon)return err(C::kNoRestorableLeadingSourceGap,m.segments().empty()?id:m.segments().front().id);for(auto&s:m.segments())if(s.id==id)return err(C::kDuplicateSegmentID,id);auto v=segments(p,m);double gap=m.segments().front().source_start;auto first=m.segments().front().id;v.insert(v.begin(),{id,0,gap,1});auto q=p;q.source_sequence=edit(std::move(v));if(auto o=p.primary_segment_audio_overrides.find(first);o!=p.primary_segment_audio_overrides.end())q.primary_segment_audio_overrides[id]=o->second;insert(q,0,gap);if(auto e=validate(q,d))return *e;return q;}
R ProjectTimelineEditing::restore_primary_trailing_gap(const RecorderProject&p,const Uuid&id,double d){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);if(m.segments().empty()||d-m.segments().back().source_end()<=kEpsilon)return err(C::kNoRestorableTrailingSourceGap,m.segments().empty()?id:m.segments().back().id);for(auto&s:m.segments())if(s.id==id)return err(C::kDuplicateSegmentID,id);auto v=segments(p,m);auto last=m.segments().back();v.push_back({id,last.source_end(),d-last.source_end(),1});auto q=p;q.source_sequence=edit(std::move(v));if(auto o=p.primary_segment_audio_overrides.find(last.id);o!=p.primary_segment_audio_overrides.end())q.primary_segment_audio_overrides[id]=o->second;if(auto e=validate(q,d))return *e;return q;}
R ProjectTimelineEditing::merge_adjacent_primary_segments(const RecorderProject&p,const Uuid&a,const Uuid&b,double d){if(auto e=validate(p,d))return *e;TimelineMap m;TimelineMap::create(p.source_sequence,d,m);auto i=std::find_if(m.segments().begin(),m.segments().end(),[&](auto&s){return s.id==a;});if(i==m.segments().end()||i+1==m.segments().end()||(i+1)->id!=b)return err(C::kRestoreJunctionNotFound,a,b);if(std::abs((i+1)->source_start-i->source_end())>kEpsilon)return err(C::kCannotMergeDeletedGap,a,b);if(std::abs((i+1)->playback_rate-i->playback_rate)>kEpsilon)return err(C::kCannotMergeDifferentPlaybackRates,a,b);if(primary_segment_audio_overrides(p,a)!=primary_segment_audio_overrides(p,b))return err(C::kCannotMergeDifferentAudioOverrides,a,b);auto v=segments(p,m);auto n=i-m.segments().begin();v[n].source_duration=(i+1)->source_end()-i->source_start;v.erase(v.begin()+n+1);auto q=p;q.source_sequence=edit(std::move(v));q.primary_segment_audio_overrides.erase(b);if(auto e=validate(q,d))return *e;return q;}
template<class X> R insert_clip(const RecorderProject&p,const X&c,std::vector<X> RecorderProject::*field,T track){for(auto&x:p.*field)if(x.id==c.id)return err(C::kDuplicateClipID,c.id,{},track);auto q=p;(q.*field).push_back(c);if(auto e=ProjectTimelineEditing::validate(q))return *e;return q;}
template<class X> R replace_clip(const RecorderProject&p,const Uuid&id,const X&c,std::vector<X> RecorderProject::*field,T track){if(id!=c.id)return err(C::kMismatchedClipID,id,c.id,track);auto q=p;auto i=std::find_if((q.*field).begin(),(q.*field).end(),[&](auto&x){return x.id==id;});if(i==(q.*field).end())return err(C::kMissingClip,id,{},track);*i=c;if(auto e=ProjectTimelineEditing::validate(q))return *e;return q;}
template<class X> R remove_overlay(const RecorderProject&p,const Uuid&id,std::vector<X> RecorderProject::*field,T track){auto q=p;auto i=std::find_if((q.*field).begin(),(q.*field).end(),[&](auto&x){return x.id==id;});if(i==(q.*field).end())return err(C::kMissingClip,id,{},track);(q.*field).erase(i);if(auto e=ProjectTimelineEditing::validate(q))return *e;return q;}
template<class X> R remove_motion(const RecorderProject&p,const Uuid&id,std::vector<X> RecorderProject::*field,T track,double def){
    auto q=p;auto& clips=q.*field;
    std::sort(clips.begin(),clips.end(),[](const auto& a,const auto& b){return a.timing.start_time==b.timing.start_time?a.id.to_string()<b.id.to_string():a.timing.start_time<b.timing.start_time;});
    auto it=std::find_if(clips.begin(),clips.end(),[&](const auto& c){return c.id==id;});if(it==clips.end())return err(C::kMissingClip,id,{},track);
    const auto index=static_cast<size_t>(it-clips.begin());clips.erase(std::remove_if(clips.begin(),clips.end(),[&](const auto& c){return c.id==id;}),clips.end());
    if(index>0){auto& t=clips[index-1].timing;const auto original=t;t.preserve_transition_intent(def);const double requested=t.requested_return_duration(def),available=index<clips.size()?clips[index].timing.start_time-t.end_time():requested;
        if(available>adjacency){t.return_duration=std::min(requested,std::max(available,0.0));if(t.return_duration>original.return_duration+0.000001)t.return_progress_offset=0;}else t=original;
    }
    if(auto e=ProjectTimelineEditing::validate(q))return *e;return q;
}
R ProjectTimelineEditing::inserting_zoom(const RecorderProject&p,const ZoomClip&c){auto r=insert_clip(p,c,&RecorderProject::zoom_clips,T::kZoom);if(auto*q=std::get_if<RecorderProject>(&r))zoom_sort(q->zoom_clips);return r;} R ProjectTimelineEditing::removing_zoom(const RecorderProject&p,const Uuid&id,TimeInterval def){auto q=p;zoom_sort(q.zoom_clips);auto i=std::find_if(q.zoom_clips.begin(),q.zoom_clips.end(),[&](auto&x){return x.id==id;});if(i==q.zoom_clips.end())return err(C::kMissingClip,id,{},T::kZoom);auto n=static_cast<std::size_t>(i-q.zoom_clips.begin());q.zoom_clips.erase(i);if(n>0){auto&before=q.zoom_clips[n-1];auto requested=before.preferred_exit_duration.value_or(before.exit_duration>kEpsilon?before.exit_duration:std::max(def,0.0));double available=n<q.zoom_clips.size()?q.zoom_clips[n].start_time-before.end_time:requested;if(available>kEpsilon){before.preferred_exit_duration=requested;before.exit_duration=std::min(requested,available);before.exit_progress_offset=0;}}if(auto e=validate(q))return *e;return q;} R ProjectTimelineEditing::replacing_zoom(const RecorderProject&p,const Uuid&id,const ZoomClip&c){auto r=replace_clip(p,id,c,&RecorderProject::zoom_clips,T::kZoom);if(auto*q=std::get_if<RecorderProject>(&r))zoom_sort(q->zoom_clips);return r;}
R ProjectTimelineEditing::inserting_screen_motion(const RecorderProject&p,const ScreenMotionClip&c){auto r=insert_clip(p,c,&RecorderProject::screen_motion_clips,T::kScreenMotion);if(auto*q=std::get_if<RecorderProject>(&r))screen_sort(q->screen_motion_clips);return r;} R ProjectTimelineEditing::removing_screen_motion(const RecorderProject&p,const Uuid&id,TimeInterval def){return remove_motion(p,id,&RecorderProject::screen_motion_clips,T::kScreenMotion,def);} R ProjectTimelineEditing::replacing_screen_motion(const RecorderProject&p,const Uuid&id,const ScreenMotionClip&c){auto r=replace_clip(p,id,c,&RecorderProject::screen_motion_clips,T::kScreenMotion);if(auto*q=std::get_if<RecorderProject>(&r))screen_sort(q->screen_motion_clips);return r;}
R ProjectTimelineEditing::inserting_camera_motion(const RecorderProject&p,const CameraMotionClip&c){auto r=insert_clip(p,c,&RecorderProject::camera_motion_clips,T::kCameraMotion);if(auto*q=std::get_if<RecorderProject>(&r))camera_sort(q->camera_motion_clips);return r;} R ProjectTimelineEditing::removing_camera_motion(const RecorderProject&p,const Uuid&id,TimeInterval def){return remove_motion(p,id,&RecorderProject::camera_motion_clips,T::kCameraMotion,def);} R ProjectTimelineEditing::replacing_camera_motion(const RecorderProject&p,const Uuid&id,const CameraMotionClip&c){auto r=replace_clip(p,id,c,&RecorderProject::camera_motion_clips,T::kCameraMotion);if(auto*q=std::get_if<RecorderProject>(&r))camera_sort(q->camera_motion_clips);return r;}
R ProjectTimelineEditing::inserting_mosaic(const RecorderProject&p,const MosaicClip&c){return insert_clip(p,c,&RecorderProject::mosaic_clips,T::kMosaic);} R ProjectTimelineEditing::removing_mosaic(const RecorderProject&p,const Uuid&id){return remove_overlay(p,id,&RecorderProject::mosaic_clips,T::kMosaic);} R ProjectTimelineEditing::replacing_mosaic(const RecorderProject&p,const Uuid&id,const MosaicClip&c){return replace_clip(p,id,c,&RecorderProject::mosaic_clips,T::kMosaic);} R ProjectTimelineEditing::inserting_sticker(const RecorderProject&p,const StickerClip&c){return insert_clip(p,c,&RecorderProject::sticker_clips,T::kSticker);} R ProjectTimelineEditing::removing_sticker(const RecorderProject&p,const Uuid&id){return remove_overlay(p,id,&RecorderProject::sticker_clips,T::kSticker);} R ProjectTimelineEditing::replacing_sticker(const RecorderProject&p,const Uuid&id,const StickerClip&c){return replace_clip(p,id,c,&RecorderProject::sticker_clips,T::kSticker);}
R ProjectTimelineEditing::setting_playback_rate(const RecorderProject&p,const Uuid&id,double requested,double full){
    if(auto e=validate(p,full))return *e;TimelineMap map;TimelineMap::create(p.source_sequence,full,map);
    auto it=std::find_if(map.segments().begin(),map.segments().end(),[&](const auto& s){return s.id==id;});if(it==map.segments().end())return err(C::kSegmentNotFound,id);
    const double rate=std::clamp(ok(requested)?requested:1.0,1.0,100.0);if(std::abs(rate-it->playback_rate)<=kEpsilon)return p;
    auto v=segments(p,map);v[static_cast<size_t>(it-map.segments().begin())].playback_rate=rate;
    const double new_end=it->output_start+it->source_duration/rate,offset=new_end-it->output_end(),scale=it->playback_rate/rate;
    const auto anchor=[&](double t){return t<=it->output_start?t:t>=it->output_end()?t+offset:it->output_start+(t-it->output_start)*scale;};
    auto q=p;q.source_sequence=edit(std::move(v));
    for(auto& c:q.zoom_clips){const double duration=c.duration();c.start_time=anchor(c.start_time);c.end_time=c.start_time+duration;}
    // 动画持续时间保留在输出时钟上，仅平移被压缩后碰撞的后继。
    for(size_t i=1;i<q.zoom_clips.size();++i){const auto& before=q.zoom_clips[i-1];auto& c=q.zoom_clips[i];if(c.start_time<before.end_time-0.0001||(c.start_time-before.end_time>adjacency&&c.start_time<before.effect_end_time()-0.0001)){const double duration=c.duration();c.start_time=c.start_time<before.end_time?before.end_time:before.effect_end_time();c.end_time=c.start_time+duration;}}
    auto transform=[&]<class X>(std::vector<X>& clips){for(auto& c:clips)c.timing.start_time=anchor(c.timing.start_time);
        if constexpr(std::is_same_v<X,ScreenMotionClip>||std::is_same_v<X,CameraMotionClip>){for(size_t i=1;i<clips.size();++i){const auto& before=clips[i-1].timing;auto& t=clips[i].timing;if(t.start_time<before.end_time()-0.0001||(t.start_time-before.end_time()>adjacency&&t.start_time<before.effect_end_time()-0.0001))t.start_time=t.start_time<before.end_time()?before.end_time():before.effect_end_time();}}
        else if constexpr(std::is_same_v<X,StickerClip>){for(auto& c:clips){c.enter_duration=std::min(c.enter_duration,c.timing.duration);c.exit_duration=std::min(c.exit_duration,c.timing.duration);}}
    };
    transform(q.screen_motion_clips);transform(q.camera_motion_clips);transform(q.mosaic_clips);transform(q.sticker_clips);
    zoom_sort(q.zoom_clips);screen_sort(q.screen_motion_clips);camera_sort(q.camera_motion_clips);
    std::sort(q.mosaic_clips.begin(),q.mosaic_clips.end(),[](const auto& a,const auto& b){return a.timing.start_time==b.timing.start_time?a.id.to_string()<b.id.to_string():a.timing.start_time<b.timing.start_time;});
    std::sort(q.sticker_clips.begin(),q.sticker_clips.end(),[](const auto& a,const auto& b){return a.layer_index==b.layer_index?a.id.to_string()<b.id.to_string():a.layer_index<b.layer_index;});
    if(auto e=validate(q,full))return *e;return q;
}
R ProjectTimelineEditing::repairing_persisted_ripple_transitions(const RecorderProject&p,TimeInterval def){
    if(p.source_sequence.is_full_recording||p.source_sequence.segments.size()<2)return p;
    auto q=p;double output=0;std::vector<double> gaps;
    for(size_t i=0;i<q.source_sequence.segments.size();++i){output+=q.source_sequence.segments[i].output_duration();if(i+1<q.source_sequence.segments.size()&&q.source_sequence.segments[i+1].source_start-q.source_sequence.segments[i].source_end()>kEpsilon)gaps.push_back(output);}
    const double safe=std::max(def,0.0);
    auto repair=[&]<class X>(std::vector<X>& clips){for(size_t i=0;i<clips.size();++i){
        if constexpr(std::is_same_v<X,ZoomClip>){auto& c=clips[i];const bool touches=i+1<clips.size()&&std::abs(clips[i+1].start_time-c.end_time)<=adjacency;
            if(!touches&&c.exit_duration<=kEpsilon&&c.exit_progress_offset>=1-kEpsilon&&std::any_of(gaps.begin(),gaps.end(),[&](double x){return std::abs(x-c.end_time)<=adjacency;})){c.exit_duration=std::min(safe,i+1<clips.size()?std::max(clips[i+1].start_time-c.end_time,0.0):safe);c.exit_progress_offset=0;}
        }else{auto& t=clips[i].timing;const bool touches=i+1<clips.size()&&std::abs(clips[i+1].timing.start_time-t.end_time())<=adjacency;
            if(!touches&&t.return_duration<=kEpsilon&&t.return_progress_offset>=1-kEpsilon&&std::any_of(gaps.begin(),gaps.end(),[&](double x){return std::abs(x-t.end_time())<=adjacency;})){t.return_duration=std::min(safe,i+1<clips.size()?std::max(clips[i+1].timing.start_time-t.end_time(),0.0):safe);t.return_progress_offset=0;}
        }
    }};
    zoom_sort(q.zoom_clips);screen_sort(q.screen_motion_clips);camera_sort(q.camera_motion_clips);repair(q.zoom_clips);repair(q.screen_motion_clips);repair(q.camera_motion_clips);return q;
}
}  // namespace dogsc
