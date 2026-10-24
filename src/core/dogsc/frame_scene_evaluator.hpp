#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "dogsc/core_types.hpp"
#include "dogsc/frame_compositor.hpp"
#include "dogsc/project_timeline.hpp"
#include "dogsc/timeline_map.hpp"

#include "dogsc/frame_tracks.hpp"

namespace dogsc {
struct RecorderProject;


/// 开场动画元素。顺序即「聚拢 / 侧滑 / 轻 3D」的入场次序。
enum class OpeningSequenceElement {
    kScreen,
    kCamera,
    kStickers,
};

enum class OpeningSequencePreset {
    kConverge,
    kSideSlide,
    kLight3D,
};

/// 开场动画的时间参数与参与元素。
///
/// 参与名单必须在整个开场时长内保持稳定，
/// 否则中途出现的贴图会改变此前元素的延迟与时长。
struct OpeningSequence {
    /// 每个参与元素在开场窗口内至少保留的动画时长。
    static constexpr TimeInterval kMinimumElementDuration = 0.18;

    bool is_enabled = false;
    OpeningSequencePreset preset = OpeningSequencePreset::kLight3D;
    ElementMotionCurve motion_curve = ElementMotionCurve::kSwift;
    TimeInterval duration = 2.2;
    TimeInterval stagger = 0.16;
    std::vector<OpeningSequenceElement> included_elements{OpeningSequenceElement::kScreen, OpeningSequenceElement::kCamera, OpeningSequenceElement::kStickers};
    std::vector<OpeningSequenceElement> element_order{OpeningSequenceElement::kScreen, OpeningSequenceElement::kCamera, OpeningSequenceElement::kStickers};

    constexpr bool operator==(const OpeningSequence&) const = default;

    [[nodiscard]] TimeInterval maximum_stagger(size_t element_count) const;
    [[nodiscard]] std::vector<OpeningSequenceElement> available_elements(
        bool has_camera, bool has_opening_sticker) const;
    [[nodiscard]] std::vector<OpeningSequenceElement> participating_elements(
        bool has_camera, bool has_opening_sticker) const;
};

/// 开场动画在某一时刻的采样，作用于元素的基础几何之上。
struct OpeningSample {
    double opacity = 1;
    double scale = 1;
    CompositionPoint offset{};
    double rotation_x = 0;
    double rotation_y = 0;
    double perspective = 0;
    double remaining = 0;
};

/// 唯一的逐帧解释器。
struct FrameEvaluationInputs {
    CompositionSize screen_source_size{};
    std::optional<CompositionSize> camera_source_size;
    double source_aspect_ratio = 16.0 / 9.0;
    const ProjectPointerTrack* pointer_track = nullptr;
    std::optional<PointerTrackEvaluation> pointer_evaluation;
    const CursorAssetMetrics* cursor_metrics = nullptr;
    const std::vector<std::pair<CursorAssetID, CursorAssetMetrics>>* cursor_metrics_by_asset = nullptr;
    const ZoomAnimationTrack* zoom_track = nullptr;
    const ScreenMotionTrack* screen_motion_track = nullptr;
    const CameraMotionTrack* camera_motion_track = nullptr;
    const StickerTransitionTrack* sticker_track = nullptr;
};

///
/// 渲染后端只接收它产出的不可变计划，绝不读取工程模型。
class FrameSceneEvaluator {
public:
    /// 计算某个元素的入场采样。`time` 已钳制在 [0, output_duration] 内。
    static OpeningSample opening_sample(const OpeningSequence& sequence,
                                        OpeningSequenceElement element,
                                        const std::vector<OpeningSequenceElement>& active_elements,
                                        TimeInterval time, double canvas_width,
                                        double canvas_height);

    /// 按采样缩放并平移一个矩形（保持中心）。
    static CompositionRect opening_rect(const CompositionRect& rect, const OpeningSample& sample);

    /// 红色标注（模糊 / 聚光）在 enter/exit 窗口上的可见度。
    /// 普通标注默认无转场；线性与平滑转场在预览与导出中求值一致。
    static double overlay_effect_progress(const OverlayTiming& timing, int transition_style,
                                          TimeInterval enter_duration,
                                          TimeInterval exit_duration, TimeInterval time);

    /// 构建一帧的渲染计划。
    ///
    /// `layer_order` 会按「光标是否可见 / 是否有聚光 / 摄像头不透明度」派生，
    /// 并保证是 `is_valid_layer_order` 接受的规范顺序子序列。
    static FrameRenderPlan render_plan(TimeInterval presentation_time,
                                       TimeInterval output_duration, int frame_rate,
                                       CompositionSize canvas_size, bool has_cursor,
                                       bool has_spotlight, bool camera_visible,
                                       bool has_stickers);
    /// 对工程模型做一次完整、不可变的逐帧解释。
    static FrameScene scene(const RecorderProject& project, TimeInterval time,
                            CompositionSize canvas_size, const FrameEvaluationInputs& inputs = {},
                            std::optional<TimeInterval> output_duration = {},
                            FrameColorContract color = FrameColorContract::sdr_desktop());
    static FrameRenderPlan render_plan(const RecorderProject& project, TimeInterval presentation_time,
                                       TimeInterval output_duration, int frame_rate,
                                       CompositionSize canvas_size,
                                       const FrameEvaluationInputs& inputs = {},
                                       FrameColorContract color = FrameColorContract::sdr_desktop());

    /// 平滑阶跃（smootherstep），用于贴图弹出与转场。
    static double smoother_step(double value);
};

}  // namespace dogsc
