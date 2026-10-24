#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "dogsc/frame_compositor.hpp"

namespace dogsc {
struct RecorderProject;
struct MotionStyle;
struct ZoomClip;

struct ScreenAnchorViewport {
    double canvas_width = 0, canvas_height = 0;
    double fitted_width = 0, fitted_height = 0;
    double border_width = 0;
    ScreenDecorationInsets decoration{};
};
struct ScreenMotionSample {
    ScreenMotionState state{};
    CompositionPoint manual_offset{};
    NormalizedPoint projection_anchor{0.5, 0.5};
};
struct CameraMotionSample {
    NormalizedPoint position{};
    double size = 0;
    double aspect_ratio = 1;
    double corner_fraction = 0;
    double fullscreen_progress = 0;
    double opacity = 1;
};
struct ManualZoomViewportTransition {
    double from_scale = 1, to_scale = 1;
    NormalizedPoint from_focus{}, to_focus{};
    double progress = 0;
};
struct ZoomSample {
    double scale = 1;
    NormalizedPoint focus{0.5, 0.5};
    double framing_progress = 0;
    double target_scale = 1;
    std::optional<ManualZoomViewportTransition> manual_viewport_transition;
};
struct ZoomViewportTransform {
    double scale = 1;
    NormalizedPoint translation{};
    [[nodiscard]] NormalizedPoint applying(NormalizedPoint point) const;
    [[nodiscard]] NormalizedPoint unapplying(NormalizedPoint point) const;
    [[nodiscard]] static ZoomViewportTransform make(const ZoomSample& sample, NormalizedCrop crop,
                                                    std::optional<CompositionRect> covering = {});
};
[[nodiscard]] double zoom_eased_progress(double value, ZoomEasingPreset easing,
                                        const ZoomBezierCurve& curve, const MotionStyle& motion);

class ScreenMotionTrack {
public:
    explicit ScreenMotionTrack(std::vector<ScreenMotionClip> clips);
    [[nodiscard]] const ScreenMotionClip* active_clip(TimeInterval time) const;
    [[nodiscard]] ScreenMotionSample sample(TimeInterval time, const ScreenMotionState& base,
                                           const ScreenAnchorViewport& viewport,
                                           const MotionStyle& motion) const;
private:
    std::vector<ScreenMotionClip> clips_;
};
class CameraMotionTrack {
public:
    explicit CameraMotionTrack(std::vector<CameraMotionClip> clips);
    [[nodiscard]] CameraMotionSample sample(TimeInterval time, const CameraMotionState& base,
                                           double camera_aspect, double canvas_aspect,
                                           const MotionStyle& motion) const;
private:
    std::vector<CameraMotionClip> clips_;
};
class ZoomAnimationTrack {
public:
    explicit ZoomAnimationTrack(const std::vector<ZoomClip>& clips,
                                std::optional<TimeInterval> output_duration = {});
    [[nodiscard]] const ZoomClip* active_clip(TimeInterval time) const;
    [[nodiscard]] const ZoomClip* adjacent_previous(const ZoomClip& clip) const;
    [[nodiscard]] ZoomSample sample(TimeInterval time, const MotionStyle& motion,
                                    std::optional<NormalizedPoint> automatic_focus = {},
                                    std::optional<NormalizedPoint> inherited_focus = {},
                                    bool reanchor_entry = false) const;
private:
    // ZoomClip 在工程头中定义；共享不可变存储使头文件无需反向包含工程。
    std::shared_ptr<const std::vector<ZoomClip>> clips_;
};
struct StickerTrackSample {
    std::vector<FrameStickerScene> stickers;
    FrameStickerBackdropScene backdrop{};
};
class StickerTransitionTrack {
public:
    explicit StickerTransitionTrack(std::vector<StickerClip> clips);
    [[nodiscard]] StickerTrackSample sample(TimeInterval time, bool suppress_initial_entry) const;
private:
    struct Entry {
        StickerClip clip;
        double enter_duration = 0, exit_duration = 0;
        std::optional<size_t> entry_join, exit_join;
    };
    struct Join {
        double start = 0, end = 0;
        FrameStickerBackdropScene outgoing{}, incoming{};
        ElementMotionCurve curve = ElementMotionCurve::kSwift;
    };
    struct Bridge {
        double start = 0, end = 0;
        FrameStickerBackdropScene treatment{};
    };
    std::vector<Entry> entries_;
    std::vector<Join> joins_;
    std::vector<Bridge> bridges_;
};

}  // namespace dogsc
