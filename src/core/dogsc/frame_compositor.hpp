#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dogsc/basic_types.hpp"
#include "dogsc/core_types.hpp"
#include "dogsc/cursor_asset.hpp"
#include "dogsc/composition_types.hpp"
#include "dogsc/scene_geometry.hpp"
#include "dogsc/project_timeline.hpp"
#include "dogsc/pointer_track.hpp"

struct ID3D11Texture2D;

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


enum class BackgroundSourceKind : int;
enum class CursorClickEffectStyle : int;

struct FrameShadow {
    HexColor color = HexColor::black();
    double opacity = 0;
    double radius = 0;
    constexpr bool operator==(const FrameShadow&) const = default;
    CompositionPoint offset{};
};

/// 已解析的背景来源，不向后端泄露工程样式。
struct FrameBackgroundSource {
    BackgroundSourceKind kind{};
    std::string pattern_preset;
    std::string dynamic_preset;
    std::string relative_path;
    std::string absolute_path;
    constexpr bool operator==(const FrameBackgroundSource&) const = default;
};
struct FrameBackgroundScene {
    FrameBackgroundSource source{};
    double blur_radius = 0;
    double pattern_scale = 1;
    double pattern_opacity = 1;
    constexpr bool operator==(const FrameBackgroundScene&) const = default;
};
enum class FrameScreenChromeKind {
    kWindow, kBrowser, kDevicePhonePortrait, kDevicePhoneLandscape,
    kDeviceTabletPortrait, kDeviceTabletLandscape,
};
struct FrameScreenChromeScene {
    FrameScreenChromeKind kind = FrameScreenChromeKind::kWindow;
    CompositionRect outer_rect{};
    CompositionRect toolbar_rect{};
    CompositionRect content_rect{};
    double outer_corner_radius = 0;
    double content_corner_radius = 0;
    HexColor surface_color{}, toolbar_color{}, separator_color{}, field_color{}, glyph_color{};
    std::string title;
    std::string browser_address;
    constexpr bool operator==(const FrameScreenChromeScene&) const = default;
};
struct FrameMosaicScene {
    NormalizedOverlayRect source_rect{};
    double corner_radius = 0;
    MosaicEffectStyle style = MosaicEffectStyle::kBlur;
    double intensity = 0;
    double spotlight_dimming = 0;
    double transition_progress = 1;
    constexpr bool operator==(const FrameMosaicScene&) const = default;
};
struct FrameFocusScene {
    FocusEffect effect{};
    NormalizedPoint center{};
    double progress = 0;
    constexpr bool operator==(const FrameFocusScene&) const = default;
};
struct FrameScreenScene {
    NormalizedCrop source_crop{};
    CompositionRect fitted_rect{}, base_rect{}, final_rect{};
    ProjectedScreenQuad projected_quad{};
    double corner_radius = 0;
    double border_width = 0;
    HexColor border_color{};
    double border_opacity = 0;
    std::optional<FrameShadow> shadow;
    std::optional<FrameScreenChromeScene> decoration;
    std::vector<FrameMosaicScene> mosaics;
    std::optional<FrameFocusScene> focus_effect;
    double opacity = 1;
    [[nodiscard]] CompositionRect projection_rect() const {
        return decoration ? decoration->outer_rect : final_rect;
    }
    constexpr bool operator==(const FrameScreenScene&) const = default;
};
struct FrameCameraScene {
    CompositionRect rect{};
    std::optional<CameraContentFillLayout> content_fill;
    double corner_radius = 0;
    double border_width = 0;
    HexColor border_color = HexColor::white();
    std::optional<FrameShadow> shadow;
    double opacity = 1;
    bool is_mirrored = false;
    constexpr bool operator==(const FrameCameraScene&) const = default;
};
enum class FrameCursorAttachment { kScreen };
struct FrameCursorScene {
    CursorAssetID asset_id{};
    CursorAssetMetrics metrics{};
    NormalizedPoint normalized_screen_position{};
    CursorRenderLayout layout{};
    bool is_clicking = false;
    std::optional<PointerClickPhase> click_phase;
    std::optional<CursorClickGeometryFrame> click_geometry;
    CursorClickEffectStyle click_style{};
    std::optional<HexColor> click_color;
    double click_opacity = 0.8;
    double click_scale = 1;
    double rotation_radians = 0;
    std::optional<FrameShadow> shadow;
    FrameCursorAttachment attachment = FrameCursorAttachment::kScreen;
    [[nodiscard]] HexColor effective_click_color() const {
        return click_color.value_or(metrics.click_color);
    }
    constexpr bool operator==(const FrameCursorScene&) const = default;
};
struct FrameStickerScene {
    Uuid id{};
    std::string relative_path;
    NormalizedPoint position{};
    double width = 0;
    double rotation_radians = 0;
    double opacity = 0;
    double scale = 1;
    NormalizedPoint offset{};
    double corner_radius = 0;
    double border_width = 0;
    HexColor border_color{};
    double shadow_opacity = 0;
    double shadow_radius = 0;
    CompositionPoint shadow_offset{};
    double backdrop_blur = 0;
    bool backdrop_blur_includes_camera = false;
    bool hides_screen = false;
    bool hides_camera = false;
    double transition_progress = 1;
    int layer_index = 0;
    constexpr bool operator==(const FrameStickerScene&) const = default;
};
struct FrameStickerBackdropScene {
    double screen_suppression = 0;
    double camera_suppression = 0;
    double screen_blur = 0;
    double camera_blur = 0;
    constexpr bool operator==(const FrameStickerBackdropScene&) const = default;
};

/// 一帧的完整、不可变解释结果。
///
/// 渲染器可以读取媒体帧与本结构，但**绝不能**直接读取工程模型，
/// 否则预览与导出会开始产生分歧。
struct FrameScene {
    TimeInterval time = 0;
    CompositionSize canvas_size{};
    FrameColorContract color = FrameColorContract::sdr_desktop();
    FrameBackgroundScene background{};
    FrameScreenScene screen{};
    std::optional<FrameCameraScene> camera;
    std::optional<FrameCursorScene> cursor;
    std::vector<FrameStickerScene> stickers;
    FrameStickerBackdropScene sticker_backdrop{};
    std::vector<FrameLayerRole> layer_order;
    constexpr bool operator==(const FrameScene&) const = default;
};

/// 一次输出帧请求。预览与导出消费同一份渲染计划。
struct FrameRenderPlan {
    TimeInterval presentation_time = 0;
    TimeInterval output_duration = 0;
    int frame_rate = 60;
    FrameScene scene{};
    constexpr bool operator==(const FrameRenderPlan&) const = default;
};

/// 校验图层顺序是否为「background → screen → cursor → spotlight → camera → stickers」
/// 的子序列且无重复。所有后端在绘制前必须调用。
[[nodiscard]] bool is_valid_layer_order(const std::vector<FrameLayerRole>& order);

/// 外部媒体只借用，不接管 COM 引用。集合与纹理必须存活到 compose 返回。
/// 纹理必须属于后端公开的 D3D11 设备，格式为 BGRA8/RGBA8 sRGB SDR。
/// 输入采用预乘 alpha，单层/单采样/单 mip，且具备 D3D11_BIND_SHADER_RESOURCE。
/// 可见 screen/spotlight 请求必须提供 screen；camera/cursor/缺失贴纸按上游跳过。
/// 仅媒体背景在 wallpaper 缺席时使用上游约定的 #2D3342 默认背景。
struct FrameRenderResources {
    ID3D11Texture2D* screen = nullptr;
    ID3D11Texture2D* camera = nullptr;
    ID3D11Texture2D* wallpaper = nullptr;
    ID3D11Texture2D* cursor = nullptr;
    std::unordered_map<std::string, ID3D11Texture2D*> stickers;
};

/// 合成器抽象，预览与导出共享资源契约和不可变渲染计划。
///
/// 实现方必须：
/// 1. 严格按 `plan.scene.layer_order` 绘制，不得重排；
/// 2. 不读取工程模型，只消费 `FrameRenderPlan` 与外部提供的媒体帧；
/// 3. 对同一输入产生逐像素稳定的输出（导出路径禁止非确定性抖动）。
class IFrameCompositor {
public:
    virtual ~IFrameCompositor() = default;

    /// 最近一次失败原因；有效期到下一次 compose/read_back 调用。
    [[nodiscard]] virtual std::string_view last_error() const = 0;

    /// 渲染一张离屏帧到内部目标。返回是否成功。
    /// `plan` 和 `resources` 仅在本次调用期间借用，不保留输入纹理引用。
    virtual bool compose(const FrameRenderPlan& plan, const FrameRenderResources& resources) = 0;

    /// 读取最近一次成功 `compose` 的结果尺寸。
    virtual CompositionSize output_size() const = 0;

    /// 把最近一次结果复制到调用方缓冲区（BGRA8，行优先，无额外 padding）。
    /// 后端调整 `out` 为 `width * height * 4` 字节。
    virtual bool read_back(std::vector<uint8_t>& out) = 0;
};

}  // namespace dogsc
