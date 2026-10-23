#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dogsc/basic_types.hpp"
#include "dogsc/core_types.hpp"
#include "dogsc/cursor_asset.hpp"

namespace dogsc {

/// 图层绘制顺序的唯一真相。
/// 渲染器必须按此顺序绘制，不得自行决定前后关系。
enum class FrameLayerRole {
    kBackground,
    kScreen,
    kCursor,
    kSpotlight,
    kCamera,
    kStickers,
};

/// 后端无关的尺寸。
struct CompositionSize {
    double width = 0;
    double height = 0;

    constexpr bool operator==(const CompositionSize&) const = default;

    [[nodiscard]] double short_edge() const { return std::min(width, height); }
};

/// 画布坐标点，原点在左上角。
struct CompositionPoint {
    double x = 0;
    double y = 0;

    constexpr bool operator==(const CompositionPoint&) const = default;
};

/// 画布坐标矩形，原点在左上角。
struct CompositionRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;

    constexpr bool operator==(const CompositionRect&) const = default;

    [[nodiscard]] double mid_x() const { return x + width / 2; }
    [[nodiscard]] double mid_y() const { return y + height / 2; }
};

/// 显式色彩意图。渲染后端可用不同 API，但不得自行选择设备相关默认值。
enum class FrameColorSpace {
    kLinearSRGB,
    kSRGB,
};

struct FrameColorContract {
    FrameColorSpace working_space = FrameColorSpace::kLinearSRGB;
    FrameColorSpace output_space = FrameColorSpace::kSRGB;
    bool is_hdr = false;

    constexpr bool operator==(const FrameColorContract&) const = default;

    /// 桌面 SDR 采集走显式 sRGB 转换；
    /// Rec.709 与 sRGB 共享原色但不共享传递曲线，不得只靠元数据改写。
    static constexpr FrameColorContract sdr_desktop() { return {}; }
};

/// 一帧的完整、不可变解释结果。
///
/// 渲染器可以读取媒体帧与本结构，但**绝不能**直接读取工程模型，
/// 否则预览与导出会开始产生分歧。
struct FrameScene {
    TimeInterval time = 0;
    CompositionSize canvas_size{};
    FrameColorContract color = FrameColorContract::sdr_desktop();
    std::vector<FrameLayerRole> layer_order;
};

/// 一次输出帧请求。预览与导出消费同一份渲染计划。
struct FrameRenderPlan {
    TimeInterval presentation_time = 0;
    TimeInterval output_duration = 0;
    int frame_rate = 60;
    FrameScene scene{};
};

/// 校验图层顺序是否为「background → screen → cursor → spotlight → camera → stickers」
/// 的子序列且无重复。所有后端在绘制前必须调用。
[[nodiscard]] bool is_valid_layer_order(const std::vector<FrameLayerRole>& order);

/// 合成器抽象。M0 只定义契约，M2a 提供 Direct2D 实现，M2b 提供预览实现。
///
/// 实现方必须：
/// 1. 严格按 `plan.scene.layer_order` 绘制，不得重排；
/// 2. 不读取工程模型，只消费 `FrameRenderPlan` 与外部提供的媒体帧；
/// 3. 对同一输入产生逐像素稳定的输出（导出路径禁止非确定性抖动）。
class IFrameCompositor {
public:
    virtual ~IFrameCompositor() = default;

    /// 渲染一张离屏帧到内部目标。返回是否成功。
    /// `plan` 的生命周期只到本次调用结束。
    virtual bool compose(const FrameRenderPlan& plan) = 0;

    /// 读取最近一次成功 `compose` 的结果尺寸。
    virtual CompositionSize output_size() const = 0;

    /// 把最近一次结果复制到调用方缓冲区（BGRA8，行优先，无额外 padding）。
    /// `out` 必须至少 `width * height * 4` 字节。
    virtual bool read_back(std::vector<uint8_t>& out) = 0;
};

}  // namespace dogsc
