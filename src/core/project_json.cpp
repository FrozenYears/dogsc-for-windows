#include "dogsc/project_json.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

#include <nlohmann/json.hpp>

namespace dogsc {
namespace {

using json = nlohmann::ordered_json;

/// 读取可选数值字段，非有限值时回落到默认值。
double finite_or(const json& node, const char* key, double fallback) {
    if (!node.contains(key)) return fallback;
    const json& value = node.at(key);
    if (!value.is_number()) return fallback;
    const double number = value.get<double>();
    return std::isfinite(number) ? number : fallback;
}

bool bool_or(const json& node, const char* key, bool fallback) {
    if (!node.contains(key) || !node.at(key).is_boolean()) return fallback;
    return node.at(key).get<bool>();
}

std::string string_or(const json& node, const char* key, const std::string& fallback) {
    if (!node.contains(key) || !node.at(key).is_string()) return fallback;
    return node.at(key).get<std::string>();
}

NormalizedPoint decode_point(const json& node, const char* key, NormalizedPoint fallback) {
    if (!node.contains(key) || !node.at(key).is_object()) return fallback;
    const json& point = node.at(key);
    return NormalizedPoint{finite_or(point, "x", fallback.x), finite_or(point, "y", fallback.y)};
}

NormalizedCrop decode_crop(const json& node) {
    if (!node.contains("crop") || !node.at("crop").is_object()) return NormalizedCrop::full();
    const json& crop = node.at("crop");
    NormalizedCrop value;
    value.x = finite_or(crop, "x", 0);
    value.y = finite_or(crop, "y", 0);
    value.width = finite_or(crop, "width", 1);
    value.height = finite_or(crop, "height", 1);
    return value.clamped();
}

/// 只接受 `#RRGGBB` 或 `RRGGBB`；输出统一大写，与上游 `hexString` 一致。
std::optional<HexColor> parse_hex_color(const std::string& text) {
    std::string digits = text;
    if (!digits.empty() && digits.front() == '#') digits.erase(digits.begin());
    if (digits.size() != 6) return std::nullopt;
    for (char& c : digits) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (!hex) return std::nullopt;
    }
    return HexColor{"#" + digits};
}

HexColor decode_hex_color(const json& node, const char* key, HexColor fallback) {
    if (!node.contains(key) || !node.at(key).is_string()) return fallback;
    return parse_hex_color(node.at(key).get<std::string>()).value_or(fallback);
}

std::optional<HexColor> decode_optional_hex_color(const json& node, const char* key) {
    if (!node.contains(key) || !node.at(key).is_string()) return std::nullopt;
    return parse_hex_color(node.at(key).get<std::string>());
}

// ---- 枚举落盘字符串 ----

std::string raw(ZoomEasingPreset preset) {
    switch (preset) {
        case ZoomEasingPreset::kLinear:
            return "线性";
        case ZoomEasingPreset::kQuadratic:
            return "二次方缓动";
        case ZoomEasingPreset::kCubic:
            return "三次方缓动";
        case ZoomEasingPreset::kQuintic:
            return "五次平滑";
        case ZoomEasingPreset::kSpring:
            return "弹簧";
        case ZoomEasingPreset::kCustom:
            return "自定义曲线";
    }
    return "三次方缓动";
}

ZoomEasingPreset easing_from_raw(const std::string& value) {
    if (value == "线性") return ZoomEasingPreset::kLinear;
    if (value == "二次方缓动") return ZoomEasingPreset::kQuadratic;
    if (value == "五次平滑") return ZoomEasingPreset::kQuintic;
    if (value == "弹簧") return ZoomEasingPreset::kSpring;
    if (value == "自定义曲线") return ZoomEasingPreset::kCustom;
    return ZoomEasingPreset::kCubic;
}

std::string raw(ZoomKeyframeOrigin origin) {
    return origin == ZoomKeyframeOrigin::kAutomatic ? "automatic" : "manual";
}

ZoomKeyframeOrigin origin_from_raw(const std::string& value) {
    return value == "automatic" ? ZoomKeyframeOrigin::kAutomatic : ZoomKeyframeOrigin::kManual;
}

std::string raw(ElementMotionCurve curve) {
    switch (curve) {
        case ElementMotionCurve::kSmooth:
            return "smooth";
        case ElementMotionCurve::kSwift:
            return "swift";
        case ElementMotionCurve::kGentle:
            return "gentle";
    }
    return "swift";
}

ElementMotionCurve curve_from_raw(const std::string& value) {
    if (value == "smooth") return ElementMotionCurve::kSmooth;
    if (value == "gentle") return ElementMotionCurve::kGentle;
    return ElementMotionCurve::kSwift;
}

std::string raw(OpeningSequencePreset preset) {
    switch (preset) {
        case OpeningSequencePreset::kConverge:
            return "聚拢";
        case OpeningSequencePreset::kSideSlide:
            return "侧滑";
        case OpeningSequencePreset::kLight3D:
            return "轻 3D";
    }
    return "轻 3D";
}

OpeningSequencePreset preset_from_raw(const std::string& value) {
    if (value == "聚拢") return OpeningSequencePreset::kConverge;
    if (value == "侧滑") return OpeningSequencePreset::kSideSlide;
    return OpeningSequencePreset::kLight3D;
}

std::string raw(OpeningSequenceElement element) {
    switch (element) {
        case OpeningSequenceElement::kScreen:
            return "屏幕";
        case OpeningSequenceElement::kCamera:
            return "摄像头";
        case OpeningSequenceElement::kStickers:
            return "贴图";
    }
    return "屏幕";
}

std::optional<OpeningSequenceElement> element_from_raw(const std::string& value) {
    if (value == "屏幕") return OpeningSequenceElement::kScreen;
    if (value == "摄像头") return OpeningSequenceElement::kCamera;
    if (value == "贴图") return OpeningSequenceElement::kStickers;
    return std::nullopt;
}

std::string raw(CameraShape shape) {
    switch (shape) {
        case CameraShape::kSquare:
            return "方形";
        case CameraShape::kHorizontal:
            return "横向";
        case CameraShape::kVertical:
            return "纵向";
        case CameraShape::kOriginal:
            return "原始比例";
        case CameraShape::kCircle:
            return "圆形";
    }
    return "方形";
}

/// 兼容历史值：`圆角矩形` → 横向，`连续圆角` → 方形。
CameraShape camera_shape_from_raw(const std::string& value) {
    if (value == "横向" || value == "圆角矩形") return CameraShape::kHorizontal;
    if (value == "纵向") return CameraShape::kVertical;
    if (value == "原始比例") return CameraShape::kOriginal;
    if (value == "圆形") return CameraShape::kCircle;
    return CameraShape::kSquare;
}

std::string raw(ScreenMotionStyle style) {
    return style == ScreenMotionStyle::kSmooth ? "平滑" : "聚焦";
}

ScreenMotionStyle screen_motion_from_raw(const std::string& value) {
    return value == "平滑" ? ScreenMotionStyle::kSmooth : ScreenMotionStyle::kFocused;
}

std::string raw(CursorMotionStyle style) {
    switch (style) {
        case CursorMotionStyle::kSmooth:
            return "平滑";
        case CursorMotionStyle::kMedium:
            return "适中";
        case CursorMotionStyle::kRapid:
            return "快速";
        case CursorMotionStyle::kNone:
            return "无";
    }
    return "平滑";
}

CursorMotionStyle cursor_motion_from_raw(const std::string& value) {
    if (value == "适中") return CursorMotionStyle::kMedium;
    if (value == "快速") return CursorMotionStyle::kRapid;
    if (value == "无") return CursorMotionStyle::kNone;
    return CursorMotionStyle::kSmooth;
}

std::string raw(CursorClickEffectStyle style) {
    switch (style) {
        case CursorClickEffectStyle::kRipple:
            return "ripple";
        case CursorClickEffectStyle::kGlow:
            return "glow";
        case CursorClickEffectStyle::kPulse:
            return "pulse";
        case CursorClickEffectStyle::kBurst:
            return "burst";
        case CursorClickEffectStyle::kNone:
            return "none";
    }
    return "ripple";
}

CursorClickEffectStyle click_effect_from_raw(const std::string& value) {
    if (value == "glow") return CursorClickEffectStyle::kGlow;
    if (value == "pulse") return CursorClickEffectStyle::kPulse;
    if (value == "burst") return CursorClickEffectStyle::kBurst;
    if (value == "none") return CursorClickEffectStyle::kNone;
    return CursorClickEffectStyle::kRipple;
}

std::string raw(BackgroundSourceKind kind) {
    switch (kind) {
        case BackgroundSourceKind::kPattern:
            return "pattern";
        case BackgroundSourceKind::kDynamicFlow:
            return "dynamicFlow";
        case BackgroundSourceKind::kProjectImage:
            return "projectImage";
        case BackgroundSourceKind::kSystemImage:
            return "systemImage";
        case BackgroundSourceKind::kProjectVideo:
            return "projectVideo";
        case BackgroundSourceKind::kSystemVideo:
            return "systemVideo";
    }
    return "pattern";
}

std::optional<BackgroundSourceKind> background_kind_from_raw(const std::string& value) {
    if (value == "pattern") return BackgroundSourceKind::kPattern;
    if (value == "dynamicFlow") return BackgroundSourceKind::kDynamicFlow;
    if (value == "projectImage") return BackgroundSourceKind::kProjectImage;
    if (value == "systemImage") return BackgroundSourceKind::kSystemImage;
    if (value == "projectVideo") return BackgroundSourceKind::kProjectVideo;
    if (value == "systemVideo") return BackgroundSourceKind::kSystemVideo;
    return std::nullopt;
}

// ---- 基本结构 ----

json encode_point(const NormalizedPoint& point) { return json{{"x", point.x}, {"y", point.y}}; }

json encode_crop(const NormalizedCrop& crop) {
    return json{{"x", crop.x}, {"y", crop.y}, {"width", crop.width}, {"height", crop.height}};
}

json encode_bezier(const ZoomBezierCurve& curve) {
    return json{{"x1", curve.x1}, {"y1", curve.y1}, {"x2", curve.x2}, {"y2", curve.y2}};
}

ZoomBezierCurve decode_bezier(const json& node, ZoomBezierCurve fallback) {
    if (!node.is_object()) return fallback;
    return ZoomBezierCurve{
        std::clamp(finite_or(node, "x1", fallback.x1), 0.0, 1.0),
        std::clamp(finite_or(node, "y1", fallback.y1), 0.0, 1.0),
        std::clamp(finite_or(node, "x2", fallback.x2), 0.0, 1.0),
        std::clamp(finite_or(node, "y2", fallback.y2), 0.0, 1.0),
    };
}

// ---- 录制配置 ----

std::optional<uint32_t> optional_uint32(const json& node, const char* key) {
    if (!node.contains(key)) return std::nullopt;
    const json& value = node.at(key);
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number <= std::numeric_limits<uint32_t>::max()) {
            return static_cast<uint32_t>(number);
        }
    }
    if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if (number >= 0 && number <= std::numeric_limits<uint32_t>::max()) {
            return static_cast<uint32_t>(number);
        }
    }
    return std::nullopt;
}

std::optional<std::string> optional_string(const json& node, const char* key) {
    if (!node.contains(key) || !node.at(key).is_string()) return std::nullopt;
    return node.at(key).get<std::string>();
}

std::optional<int> optional_int(const json& node, const char* key) {
    if (!node.contains(key) || !node.at(key).is_number_integer()) return std::nullopt;
    const json& value = node.at(key);
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number > static_cast<uint64_t>(std::numeric_limits<int>::max())) return std::nullopt;
        return static_cast<int>(number);
    }
    const auto number = value.get<int64_t>();
    if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(number);
}

std::optional<NormalizedRect> decode_capture_area(const json& node) {
    if (!node.is_object()) return std::nullopt;
    return NormalizedRect{finite_or(node, "x", 0), finite_or(node, "y", 0),
                          finite_or(node, "width", 1), finite_or(node, "height", 1)};
}

std::optional<CameraCaptureResolution> decode_camera_capture_resolution(const json& node) {
    if (!node.is_object()) return std::nullopt;
    const auto width = optional_int(node, "width");
    const auto height = optional_int(node, "height");
    if (!width || !height) return std::nullopt;
    return CameraCaptureResolution{*width, *height};
}

std::optional<OutputFrameRate> capture_frame_rate_from_raw(const json& node, const char* key) {
    const auto value = optional_int(node, key);
    if (!value) return std::nullopt;
    switch (static_cast<OutputFrameRate>(*value)) {
        case OutputFrameRate::fps30:
        case OutputFrameRate::fps60:
        case OutputFrameRate::fps90:
        case OutputFrameRate::fps120:
            return static_cast<OutputFrameRate>(*value);
    }
    return std::nullopt;
}

CaptureConfiguration decode_capture(const json& node) {
    CaptureConfiguration capture;
    if (!node.is_object()) return capture;

    capture.source = capture_source_from_raw(string_or(node, "source", "整块屏幕"))
                         .value_or(CaptureSource::kDisplay);
    capture.display_id = optional_uint32(node, "displayID");
    capture.display_name = optional_string(node, "displayName");
    capture.window_id = optional_uint32(node, "windowID");
    capture.window_name = optional_string(node, "windowName");
    capture.selected_application_bundle_identifier =
        optional_string(node, "selectedApplicationBundleIdentifier");
    capture.selected_application_name = optional_string(node, "selectedApplicationName");
    if (node.contains("area")) capture.area = decode_capture_area(node.at("area"));
    capture.device_id = optional_string(node, "deviceID");
    capture.device_name = optional_string(node, "deviceName");
    capture.capture_frame_rate =
        capture_frame_rate_from_raw(node, "captureFrameRate").value_or(OutputFrameRate::fps60);
    // 新建工程默认 HEVC；与 Swift 的对象解码一致，旧 capture 对象缺失 codec 时回落 H.264。
    capture.capture_codec =
        capture_codec_from_raw(string_or(node, "captureCodec", "H.264"))
            .value_or(CaptureCodec::kH264);
    capture.capture_resolution_limit =
        capture_resolution_limit_from_raw(string_or(node, "captureResolutionLimit", "原始分辨率"))
            .value_or(CaptureResolutionLimit::kNative);
    capture.hides_desktop_files = bool_or(node, "hidesDesktopFiles", false);
    capture.hides_dock = bool_or(node, "hidesDock", false);
    capture.records_system_audio = bool_or(node, "recordsSystemAudio", true);
    capture.system_audio_scope =
        system_audio_scope_from_raw(string_or(node, "systemAudioScope", "全部系统声音"))
            .value_or(SystemAudioScope::kAll);
    capture.records_microphone = bool_or(node, "recordsMicrophone", false);
    capture.microphone_device_id = optional_string(node, "microphoneDeviceID");
    capture.microphone_device_name = optional_string(node, "microphoneDeviceName");
    capture.records_camera = bool_or(node, "recordsCamera", false);
    capture.camera_device_id = optional_string(node, "cameraDeviceID");
    capture.camera_device_name = optional_string(node, "cameraDeviceName");
    if (node.contains("cameraCaptureResolution")) {
        capture.camera_capture_resolution =
            decode_camera_capture_resolution(node.at("cameraCaptureResolution"));
    }
    return capture;
}

json encode_capture(const CaptureConfiguration& capture) {
    json node;
    node["source"] = to_raw_value(capture.source);
    if (capture.display_id) node["displayID"] = *capture.display_id;
    if (capture.display_name) node["displayName"] = *capture.display_name;
    if (capture.window_id) node["windowID"] = *capture.window_id;
    if (capture.window_name) node["windowName"] = *capture.window_name;
    if (capture.selected_application_bundle_identifier) {
        node["selectedApplicationBundleIdentifier"] =
            *capture.selected_application_bundle_identifier;
    }
    if (capture.selected_application_name) {
        node["selectedApplicationName"] = *capture.selected_application_name;
    }
    if (capture.area) {
        node["area"] = json{{"x", capture.area->x}, {"y", capture.area->y},
                             {"width", capture.area->width}, {"height", capture.area->height}};
    }
    if (capture.device_id) node["deviceID"] = *capture.device_id;
    if (capture.device_name) node["deviceName"] = *capture.device_name;
    node["captureFrameRate"] = static_cast<int>(capture.capture_frame_rate);
    node["captureCodec"] = to_raw_value(capture.capture_codec);
    node["captureResolutionLimit"] = to_raw_value(capture.capture_resolution_limit);
    node["hidesDesktopFiles"] = capture.hides_desktop_files;
    node["hidesDock"] = capture.hides_dock;
    node["recordsSystemAudio"] = capture.records_system_audio;
    node["systemAudioScope"] = to_raw_value(capture.system_audio_scope);
    node["recordsMicrophone"] = capture.records_microphone;
    if (capture.microphone_device_id) node["microphoneDeviceID"] = *capture.microphone_device_id;
    if (capture.microphone_device_name) {
        node["microphoneDeviceName"] = *capture.microphone_device_name;
    }
    node["recordsCamera"] = capture.records_camera;
    if (capture.camera_device_id) node["cameraDeviceID"] = *capture.camera_device_id;
    if (capture.camera_device_name) node["cameraDeviceName"] = *capture.camera_device_name;
    if (capture.camera_capture_resolution) {
        node["cameraCaptureResolution"] =
            json{{"width", capture.camera_capture_resolution->width},
                 {"height", capture.camera_capture_resolution->height}};
    }
    return node;
}

// ---- 背景 ----

BackgroundSource decode_background(const json& node) {
    BackgroundSource source = BackgroundSource::safe_fallback();
    if (!node.is_object()) return source;
    const auto kind = background_kind_from_raw(string_or(node, "kind", ""));
    if (!kind.has_value()) return source;
    source.kind = *kind;
    source.pattern_preset = string_or(node, "patternPreset", source.pattern_preset);
    source.dynamic_preset = string_or(node, "dynamicPreset", source.dynamic_preset);
    source.relative_path = string_or(node, "relativePath", source.relative_path);
    source.absolute_path = string_or(node, "absolutePath", source.absolute_path);
    return source;
}

json encode_background(const BackgroundSource& source) {
    json node{{"kind", raw(source.kind)}};
    switch (source.kind) {
        case BackgroundSourceKind::kPattern:
            node["patternPreset"] = source.pattern_preset;
            break;
        case BackgroundSourceKind::kDynamicFlow:
            node["dynamicPreset"] = source.dynamic_preset;
            break;
        case BackgroundSourceKind::kProjectImage:
        case BackgroundSourceKind::kProjectVideo:
            node["relativePath"] = source.relative_path;
            break;
        case BackgroundSourceKind::kSystemImage:
        case BackgroundSourceKind::kSystemVideo:
            node["absolutePath"] = source.absolute_path;
            break;
    }
    return node;
}


// ---- ProjectJson 入口 ----

struct CanvasDecoded {
    CanvasStyle style;
};

CanvasStyle decode_canvas(const json& node) {
    CanvasStyle style;
    if (!node.is_object()) return style;
    style.aspect_ratio = canvas_aspect_ratio_from_raw(string_or(node, "aspectRatio", "适应"))
                            .value_or(CanvasAspectRatio::kAdaptive);
    if (node.contains("customAspectRatio") && node.at("customAspectRatio").is_object()) {
        const json& custom = node.at("customAspectRatio");
        style.custom_aspect_ratio.width = finite_or(custom, "width", 16);
        style.custom_aspect_ratio.height = finite_or(custom, "height", 9);
    }
    if (node.contains("backgroundSource")) {
        style.background_source = decode_background(node.at("backgroundSource"));
    }
    style.padding = finite_or(node, "padding", 100);
    style.content_scale = finite_or(node, "contentScale", 1);
    style.content_position = decode_point(node, "contentPosition", {0.5, 0.5});
    style.crop = decode_crop(node);
    style.corner_radius = finite_or(node, "cornerRadius", 24);
    style.border_width = finite_or(node, "borderWidth", 0);
    style.border_color = decode_hex_color(node, "borderHex", HexColor::white());
    style.shadow_strength = finite_or(node, "shadowStrength", 0.28);
    style.background_blur = finite_or(node, "backgroundBlur", 0);
    style.inset_opacity = finite_or(node, "insetOpacity", 0.125);
    style.screen_frame = string_or(node, "screenFrame", "none");
    style.screen_frame_scale = std::clamp(finite_or(node, "screenFrameScale", 1), 0.6, 1.6);
    style.screen_frame_toolbar_scale =
        std::clamp(finite_or(node, "screenFrameToolbarScale", 1), 0.65, 1.6);
    if (node.contains("screenFrameOuterCornerRadius") &&
        node.at("screenFrameOuterCornerRadius").is_number()) {
        style.screen_frame_outer_corner_radius =
            std::clamp(node.at("screenFrameOuterCornerRadius").get<double>(), 0.0, 160.0);
    }
    if (node.contains("screenFrameContentCornerRadius") &&
        node.at("screenFrameContentCornerRadius").is_number()) {
        style.screen_frame_content_corner_radius =
            std::clamp(node.at("screenFrameContentCornerRadius").get<double>(), 0.0, 160.0);
    }
    style.screen_frame_title = string_or(node, "screenFrameTitle", "").substr(0, 120);
    style.screen_frame_browser_address =
        string_or(node, "screenFrameBrowserAddress", "").substr(0, 240);
    style.pattern_scale = std::clamp(finite_or(node, "patternScale", 1), 0.4, 6.0);
    style.pattern_opacity = std::clamp(finite_or(node, "patternOpacity", 1), 0.0, 1.0);
    return style;
}

json encode_canvas(const CanvasStyle& style) {
    json node;
    node["aspectRatio"] = to_raw_value(style.aspect_ratio);
    node["customAspectRatio"] = json{{"width", style.custom_aspect_ratio.width},
                                     {"height", style.custom_aspect_ratio.height}};
    node["backgroundSource"] = encode_background(style.background_source);
    node["padding"] = style.padding;
    node["contentScale"] = style.content_scale;
    node["contentPosition"] = encode_point(style.content_position);
    node["crop"] = encode_crop(style.crop);
    node["cornerRadius"] = style.corner_radius;
    node["borderWidth"] = style.border_width;
    node["borderHex"] = style.border_color.value;
    node["shadowStrength"] = style.shadow_strength;
    node["backgroundBlur"] = style.background_blur;
    node["insetOpacity"] = style.inset_opacity;
    node["screenFrame"] = style.screen_frame;
    node["screenFrameScale"] = style.screen_frame_scale;
    node["screenFrameToolbarScale"] = style.screen_frame_toolbar_scale;
    if (style.screen_frame_outer_corner_radius) {
        node["screenFrameOuterCornerRadius"] = *style.screen_frame_outer_corner_radius;
    }
    if (style.screen_frame_content_corner_radius) {
        node["screenFrameContentCornerRadius"] = *style.screen_frame_content_corner_radius;
    }
    node["screenFrameTitle"] = style.screen_frame_title;
    node["screenFrameBrowserAddress"] = style.screen_frame_browser_address;
    node["patternScale"] = style.pattern_scale;
    node["patternOpacity"] = style.pattern_opacity;
    return node;
}

void append_unique(std::vector<OpeningSequenceElement>& list, OpeningSequenceElement element) {
    if (std::find(list.begin(), list.end(), element) == list.end()) list.push_back(element);
}

OpeningSequence decode_opening(const json& node) {
    OpeningSequence sequence;
    if (!node.is_object()) return sequence;
    sequence.is_enabled = bool_or(node, "isEnabled", false);
    sequence.preset = preset_from_raw(string_or(node, "preset", "轻 3D"));
    sequence.motion_curve = curve_from_raw(string_or(node, "motionCurve", "swift"));
    sequence.duration = std::clamp(finite_or(node, "duration", 2.2), 0.4, 8.0);
    if (node.contains("includedElements") && node.at("includedElements").is_array()) {
        sequence.included_elements.clear();
        for (const json& item : node.at("includedElements")) {
            if (!item.is_string()) continue;
            const auto element = element_from_raw(item.get<std::string>());
            if (element.has_value()) append_unique(sequence.included_elements, *element);
        }
    }
    if (node.contains("elementOrder") && node.at("elementOrder").is_array()) {
        sequence.element_order.clear();
        for (const json& item : node.at("elementOrder")) {
            if (!item.is_string()) continue;
            const auto element = element_from_raw(item.get<std::string>());
            if (element.has_value()) append_unique(sequence.element_order, *element);
        }
        for (OpeningSequenceElement element :
             {OpeningSequenceElement::kScreen, OpeningSequenceElement::kCamera,
              OpeningSequenceElement::kStickers}) {
            append_unique(sequence.element_order, element);
        }
    }
    const double max_stagger = sequence.maximum_stagger(sequence.included_elements.size());
    sequence.stagger = std::clamp(finite_or(node, "stagger", 0.16), 0.0, max_stagger);
    return sequence;
}

json encode_opening(const OpeningSequence& sequence) {
    json included = json::array();
    for (OpeningSequenceElement element : sequence.included_elements) {
        included.push_back(raw(element));
    }
    json order = json::array();
    for (OpeningSequenceElement element : sequence.element_order) {
        order.push_back(raw(element));
    }
    return json{{"isEnabled", sequence.is_enabled},
                {"preset", raw(sequence.preset)},
                {"motionCurve", raw(sequence.motion_curve)},
                {"duration", sequence.duration},
                {"stagger", sequence.stagger},
                {"includedElements", included},
                {"elementOrder", order}};
}

std::string raw(FocusEffectShape shape) {
    return shape == FocusEffectShape::kLinear ? "linear" : "radial";
}

std::string raw(FocusEffectTarget target) {
    switch (target) {
        case FocusEffectTarget::kAnimation:
            return "animation";
        case FocusEffectTarget::kPointer:
            return "pointer";
        case FocusEffectTarget::kFixed:
            return "fixed";
    }
    return "animation";
}

json encode_focus_effect(const FocusEffect& effect) {
    json node{{"target", raw(effect.target)},
              {"center", encode_point(effect.center)},
              {"size", effect.size},
              {"blur", effect.blur},
              {"softness", effect.softness},
              {"dimming", effect.dimming}};
    if (effect.shape) node["shape"] = raw(*effect.shape);
    if (effect.angle_degrees) node["angleDegrees"] = *effect.angle_degrees;
    return node;
}

FocusEffect decode_focus_effect(const json& node) {
    FocusEffect effect;
    if (!node.is_object()) return effect;
    const std::string shape = string_or(node, "shape", "");
    if (shape == "radial") effect.shape = FocusEffectShape::kRadial;
    else if (shape == "linear") effect.shape = FocusEffectShape::kLinear;
    if (node.contains("angleDegrees") && node.at("angleDegrees").is_number()) {
        const double angle = node.at("angleDegrees").get<double>();
        if (std::isfinite(angle)) effect.angle_degrees = angle;
    }
    const std::string target = string_or(node, "target", "animation");
    if (target == "pointer") effect.target = FocusEffectTarget::kPointer;
    else if (target == "fixed") effect.target = FocusEffectTarget::kFixed;
    effect.center = decode_point(node, "center", {0.5, 0.5});
    effect.size = finite_or(node, "size", 0.45);
    effect.blur = finite_or(node, "blur", 0.35);
    effect.softness = finite_or(node, "softness", 0.5);
    effect.dimming = finite_or(node, "dimming", 0.18);
    return effect;
}

json encode_transition_timing(const TransitionTiming& timing) {
    json node{{"startTime", timing.start_time},
              {"duration", timing.duration},
              {"leadInDuration", timing.lead_in_duration},
              {"leadInProgressOffset", timing.lead_in_progress_offset},
              {"easing", raw(timing.easing)},
              {"customCurve", encode_bezier(timing.custom_curve)},
              {"returnDuration", timing.return_duration},
              {"returnProgressOffset", timing.return_progress_offset}};
    if (timing.preferred_lead_in_duration) {
        node["preferredLeadInDuration"] = *timing.preferred_lead_in_duration;
    }
    if (timing.preferred_return_duration) {
        node["preferredReturnDuration"] = *timing.preferred_return_duration;
    }
    return node;
}

TransitionTiming decode_transition_timing(const json& node) {
    TransitionTiming timing;
    if (!node.is_object()) return timing;
    timing.start_time = finite_or(node, "startTime", 0);
    timing.duration = finite_or(node, "duration", 0.7);
    timing.lead_in_duration = std::clamp(finite_or(node, "leadInDuration", 0.55), 0.0, 5.0);
    timing.lead_in_progress_offset =
        std::clamp(finite_or(node, "leadInProgressOffset", 0), 0.0, 1.0);
    timing.easing = easing_from_raw(string_or(node, "easing", "三次方缓动"));
    if (node.contains("customCurve")) {
        timing.custom_curve = decode_bezier(node.at("customCurve"), ZoomBezierCurve::cubic());
    }
    timing.return_duration = std::clamp(finite_or(node, "returnDuration", 0), 0.0, 5.0);
    timing.return_progress_offset =
        std::clamp(finite_or(node, "returnProgressOffset", 0), 0.0, 1.0);
    if (node.contains("preferredLeadInDuration") &&
        node.at("preferredLeadInDuration").is_number()) {
        const double value = node.at("preferredLeadInDuration").get<double>();
        if (std::isfinite(value)) timing.preferred_lead_in_duration = value;
    }
    if (node.contains("preferredReturnDuration") &&
        node.at("preferredReturnDuration").is_number()) {
        const double value = node.at("preferredReturnDuration").get<double>();
        if (std::isfinite(value)) timing.preferred_return_duration = value;
    }
    return timing;
}

json encode_screen_motion_state(const ScreenMotionState& state) {
    return json{{"position", encode_point(state.position)},
                {"scale", state.scale},
                {"rotationX", state.rotation_x},
                {"rotationY", state.rotation_y},
                {"rotationZ", state.rotation_z},
                {"perspective", state.perspective}};
}

ScreenMotionState decode_screen_motion_state(const json& node) {
    ScreenMotionState state;
    if (!node.is_object()) return state;
    state.position = decode_point(node, "position", {0.5, 0.5});
    state.scale = finite_or(node, "scale", 1);
    state.rotation_x = finite_or(node, "rotationX", 0);
    state.rotation_y = finite_or(node, "rotationY", 0);
    state.rotation_z = finite_or(node, "rotationZ", 0);
    state.perspective = finite_or(node, "perspective", 0.65);
    return state;
}

std::optional<Uuid> optional_uuid(const json& node, const char* key) {
    if (!node.contains(key) || !node.at(key).is_string()) return std::nullopt;
    return Uuid::parse(node.at(key).get<std::string>());
}

ScreenMotionClip decode_screen_motion_clip(const json& node) {
    ScreenMotionClip clip;
    if (!node.is_object()) return clip;
    if (const auto id = optional_uuid(node, "id")) clip.id = *id;
    if (node.contains("timing")) clip.timing = decode_transition_timing(node.at("timing"));
    if (node.contains("target")) clip.target = decode_screen_motion_state(node.at("target"));
    clip.group_id = optional_uuid(node, "groupID");
    if (node.contains("focusEffect") && node.at("focusEffect").is_object()) {
        clip.focus_effect = decode_focus_effect(node.at("focusEffect"));
    }
    return clip;
}

json encode_screen_motion_clip(const ScreenMotionClip& clip) {
    json node{{"id", clip.id.to_string()},
              {"timing", encode_transition_timing(clip.timing)},
              {"target", encode_screen_motion_state(clip.target)}};
    if (clip.group_id) node["groupID"] = clip.group_id->to_string();
    if (clip.focus_effect) node["focusEffect"] = encode_focus_effect(*clip.focus_effect);
    return node;
}

json encode_camera_layout(const CameraLayoutMode& layout) {
    if (layout.kind == CameraLayoutMode::Kind::kFullscreen) {
        return json{{"kind", "fullscreen"}};
    }
    return json{{"kind", "shape"}, {"shape", raw(layout.shape)}};
}

CameraLayoutMode decode_camera_layout(const json& node) {
    if (!node.is_object()) return {};
    if (string_or(node, "kind", "shape") == "fullscreen") {
        return CameraLayoutMode::fullscreen();
    }
    return CameraLayoutMode::shaped(
        camera_shape_from_raw(string_or(node, "shape", "方形")));
}

json encode_camera_motion_state(const CameraMotionState& state) {
    return json{{"layout", encode_camera_layout(state.layout)},
                {"position", encode_point(state.position)},
                {"size", state.size},
                {"roundness", state.roundness},
                {"opacity", state.opacity}};
}

CameraMotionState decode_camera_motion_state(const json& node) {
    CameraMotionState state;
    if (!node.is_object()) return state;
    if (node.contains("layout")) state.layout = decode_camera_layout(node.at("layout"));
    state.position = decode_point(node, "position", {0.86, 0.82});
    state.size = finite_or(node, "size", 0.35);
    state.roundness = finite_or(node, "roundness", 0.5);
    state.opacity = finite_or(node, "opacity", 1);
    return state;
}

CameraMotionClip decode_camera_motion_clip(const json& node) {
    CameraMotionClip clip;
    if (!node.is_object()) return clip;
    if (const auto id = optional_uuid(node, "id")) clip.id = *id;
    if (node.contains("timing")) clip.timing = decode_transition_timing(node.at("timing"));
    if (node.contains("target")) clip.target = decode_camera_motion_state(node.at("target"));
    clip.group_id = optional_uuid(node, "groupID");
    return clip;
}

json encode_camera_motion_clip(const CameraMotionClip& clip) {
    json node{{"id", clip.id.to_string()},
              {"timing", encode_transition_timing(clip.timing)},
              {"target", encode_camera_motion_state(clip.target)}};
    if (clip.group_id) node["groupID"] = clip.group_id->to_string();
    return node;
}

json encode_overlay_timing(const OverlayTiming& timing) {
    return json{{"startTime", timing.start_time}, {"duration", timing.duration}};
}

OverlayTiming decode_overlay_timing(const json& node) {
    OverlayTiming timing;
    if (!node.is_object()) return timing;
    timing.start_time = finite_or(node, "startTime", 0);
    timing.duration = finite_or(node, "duration", 3);
    return timing;
}

json encode_overlay_rect(const NormalizedOverlayRect& rect) {
    return json{{"x", rect.x}, {"y", rect.y}, {"width", rect.width}, {"height", rect.height}};
}

NormalizedOverlayRect decode_overlay_rect(const json& node) {
    if (!node.is_object()) return NormalizedOverlayRect::centered();
    return {finite_or(node, "x", 0.35), finite_or(node, "y", 0.39),
            finite_or(node, "width", 0.30), finite_or(node, "height", 0.22)};
}

std::string raw(MosaicEffectStyle style) {
    return style == MosaicEffectStyle::kSpotlight ? "spotlight" : "blur";
}

MosaicEffectStyle mosaic_style_from_raw(const std::string& value) {
    return value == "spotlight" ? MosaicEffectStyle::kSpotlight : MosaicEffectStyle::kBlur;
}

std::string raw(MosaicTransitionStyle style) {
    switch (style) {
        case MosaicTransitionStyle::kNone:
            return "none";
        case MosaicTransitionStyle::kLinear:
            return "linear";
        case MosaicTransitionStyle::kSmooth:
            return "smooth";
    }
    return "none";
}

MosaicTransitionStyle mosaic_transition_from_raw(const std::string& value,
                                                  MosaicTransitionStyle fallback) {
    if (value == "linear") return MosaicTransitionStyle::kLinear;
    if (value == "smooth") return MosaicTransitionStyle::kSmooth;
    if (value == "none") return MosaicTransitionStyle::kNone;
    return fallback;
}

MosaicClip decode_mosaic_clip(const json& node) {
    MosaicClip clip;
    if (!node.is_object()) return clip;
    if (const auto id = optional_uuid(node, "id")) clip.id = *id;
    if (node.contains("timing")) clip.timing = decode_overlay_timing(node.at("timing"));
    if (node.contains("sourceRect")) clip.source_rect = decode_overlay_rect(node.at("sourceRect"));
    clip.corner_radius = finite_or(node, "cornerRadius", 0.08);
    clip.style = mosaic_style_from_raw(string_or(node, "style", "blur"));
    clip.intensity = finite_or(node, "intensity", 0.55);
    clip.spotlight_dimming = finite_or(node, "spotlightDimming", 0.22);
    const auto default_transition = clip.style == MosaicEffectStyle::kSpotlight
                                       ? MosaicTransitionStyle::kSmooth
                                       : MosaicTransitionStyle::kNone;
    clip.transition_style = mosaic_transition_from_raw(
        string_or(node, "transitionStyle", raw(default_transition)), default_transition);
    clip.transition_in_duration = finite_or(node, "transitionInDuration", 0.30);
    clip.transition_out_duration = finite_or(node, "transitionOutDuration", 0.30);
    return clip;
}

json encode_mosaic_clip(const MosaicClip& clip) {
    return json{{"id", clip.id.to_string()},
                {"timing", encode_overlay_timing(clip.timing)},
                {"sourceRect", encode_overlay_rect(clip.source_rect)},
                {"cornerRadius", clip.corner_radius},
                {"style", raw(clip.style)},
                {"intensity", clip.intensity},
                {"spotlightDimming", clip.spotlight_dimming},
                {"transitionStyle", raw(clip.transition_style)},
                {"transitionInDuration", clip.transition_in_duration},
                {"transitionOutDuration", clip.transition_out_duration}};
}

std::string raw(StickerAnimationPreset preset) {
    switch (preset) {
        case StickerAnimationPreset::kNone: return "none";
        case StickerAnimationPreset::kFade: return "fade";
        case StickerAnimationPreset::kPop: return "pop";
        case StickerAnimationPreset::kSlideLeft: return "slideLeft";
        case StickerAnimationPreset::kSlideRight: return "slideRight";
        case StickerAnimationPreset::kSlideUp: return "slideUp";
        case StickerAnimationPreset::kSlideDown: return "slideDown";
        case StickerAnimationPreset::kSlideTopLeft: return "slideTopLeft";
        case StickerAnimationPreset::kSlideTopRight: return "slideTopRight";
        case StickerAnimationPreset::kSlideBottomLeft: return "slideBottomLeft";
        case StickerAnimationPreset::kSlideBottomRight: return "slideBottomRight";
    }
    return "pop";
}

StickerAnimationPreset sticker_animation_from_raw(const std::string& value) {
    if (value == "none") return StickerAnimationPreset::kNone;
    if (value == "fade") return StickerAnimationPreset::kFade;
    if (value == "slideLeft") return StickerAnimationPreset::kSlideLeft;
    if (value == "slideRight") return StickerAnimationPreset::kSlideRight;
    if (value == "slideUp") return StickerAnimationPreset::kSlideUp;
    if (value == "slideDown") return StickerAnimationPreset::kSlideDown;
    if (value == "slideTopLeft") return StickerAnimationPreset::kSlideTopLeft;
    if (value == "slideTopRight") return StickerAnimationPreset::kSlideTopRight;
    if (value == "slideBottomLeft") return StickerAnimationPreset::kSlideBottomLeft;
    if (value == "slideBottomRight") return StickerAnimationPreset::kSlideBottomRight;
    return StickerAnimationPreset::kPop;
}

StickerClip decode_sticker_clip(const json& node) {
    StickerClip clip;
    if (!node.is_object()) return clip;
    if (const auto id = optional_uuid(node, "id")) clip.id = *id;
    if (node.contains("timing")) clip.timing = decode_overlay_timing(node.at("timing"));
    clip.relative_path = string_or(node, "relativePath", "");
    clip.position = decode_point(node, "position", {0.5, 0.5});
    clip.width = finite_or(node, "width", 0.38);
    clip.rotation_degrees = finite_or(node, "rotationDegrees", 0);
    clip.opacity = finite_or(node, "opacity", 1);
    clip.corner_radius = finite_or(node, "cornerRadius", 18);
    clip.border_width = finite_or(node, "borderWidth", 0);
    clip.border_color = decode_hex_color(node, "borderColor", HexColor::white());
    clip.shadow_opacity = finite_or(node, "shadowOpacity", 0.28);
    clip.shadow_radius = finite_or(node, "shadowRadius", 24);
    clip.shadow_offset_x = finite_or(node, "shadowOffsetX", 0);
    clip.shadow_offset_y = finite_or(node, "shadowOffsetY", 10);
    clip.animation = sticker_animation_from_raw(string_or(node, "animation", "pop"));
    clip.animation_curve = curve_from_raw(string_or(node, "animationCurve", "swift"));
    if (node.contains("exitAnimation") && node.at("exitAnimation").is_string()) {
        clip.exit_animation = sticker_animation_from_raw(node.at("exitAnimation").get<std::string>());
    }
    clip.enter_duration = finite_or(node, "enterDuration", 0.7);
    clip.exit_duration = finite_or(node, "exitDuration", 0.22);
    clip.backdrop_blur = finite_or(node, "backdropBlur", 20);
    clip.backdrop_blur_includes_camera = bool_or(node, "backdropBlurIncludesCamera", false);
    clip.hides_screen = bool_or(node, "hidesScreen", false);
    clip.hides_camera = bool_or(node, "hidesCamera", false);
    if (node.contains("layerIndex") && node.at("layerIndex").is_number_integer()) {
        clip.layer_index = node.at("layerIndex").get<int>();
    }
    return clip;
}

json encode_sticker_clip(const StickerClip& clip) {
    json node{{"id", clip.id.to_string()},
              {"timing", encode_overlay_timing(clip.timing)},
              {"relativePath", clip.relative_path},
              {"position", encode_point(clip.position)},
              {"width", clip.width},
              {"rotationDegrees", clip.rotation_degrees},
              {"opacity", clip.opacity},
              {"cornerRadius", clip.corner_radius},
              {"borderWidth", clip.border_width},
              {"borderColor", clip.border_color.value},
              {"shadowOpacity", clip.shadow_opacity},
              {"shadowRadius", clip.shadow_radius},
              {"shadowOffsetX", clip.shadow_offset_x},
              {"shadowOffsetY", clip.shadow_offset_y},
              {"animation", raw(clip.animation)},
              {"animationCurve", raw(clip.animation_curve)},
              {"enterDuration", clip.enter_duration},
              {"exitDuration", clip.exit_duration},
              {"backdropBlur", clip.backdrop_blur},
              {"backdropBlurIncludesCamera", clip.backdrop_blur_includes_camera},
              {"hidesScreen", clip.hides_screen},
              {"hidesCamera", clip.hides_camera},
              {"layerIndex", clip.layer_index}};
    if (clip.exit_animation) node["exitAnimation"] = raw(*clip.exit_animation);
    return node;
}

PrimarySegmentAudioOverrides decode_audio_overrides(const json& node) {
    PrimarySegmentAudioOverrides overrides;
    if (!node.is_object()) return overrides;
    const auto optional_number = [&node](const char* key) -> std::optional<double> {
        if (!node.contains(key) || !node.at(key).is_number()) return std::nullopt;
        const double value = node.at(key).get<double>();
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    };
    overrides.system_volume = optional_number("systemVolume");
    overrides.microphone_volume = optional_number("microphoneVolume");
    if (node.contains("isSystemMuted") && node.at("isSystemMuted").is_boolean()) {
        overrides.is_system_muted = node.at("isSystemMuted").get<bool>();
    }
    if (node.contains("isMicrophoneMuted") && node.at("isMicrophoneMuted").is_boolean()) {
        overrides.is_microphone_muted = node.at("isMicrophoneMuted").get<bool>();
    }
    return overrides;
}

json encode_audio_overrides(const PrimarySegmentAudioOverrides& overrides) {
    json node = json::object();
    if (overrides.system_volume) node["systemVolume"] = *overrides.system_volume;
    if (overrides.microphone_volume) node["microphoneVolume"] = *overrides.microphone_volume;
    if (overrides.is_system_muted) node["isSystemMuted"] = *overrides.is_system_muted;
    if (overrides.is_microphone_muted) node["isMicrophoneMuted"] = *overrides.is_microphone_muted;
    return node;
}

ZoomClip decode_zoom_clip(const json& node) {
    ZoomClip clip;
    if (!node.is_object()) return clip;
    if (node.contains("id") && node.at("id").is_string()) {
        if (const auto id = Uuid::parse(node.at("id").get<std::string>())) clip.id = *id;
    }
    clip.start_time = std::max(finite_or(node, "startTime", 0), 0.0);
    clip.end_time = std::max(finite_or(node, "endTime", clip.start_time), clip.start_time);
    clip.scale = std::max(finite_or(node, "scale", 1.6), 1.0);
    clip.focus = decode_point(node, "focus", {0.5, 0.5});
    clip.origin = origin_from_raw(string_or(node, "origin", "manual"));
    clip.easing = easing_from_raw(string_or(node, "easing", "三次方缓动"));
    if (node.contains("customCurve")) {
        clip.custom_curve = decode_bezier(node.at("customCurve"), ZoomBezierCurve::cubic());
    }
    clip.enter_duration = std::clamp(finite_or(node, "enterDuration", 0.55), 0.0, 5.0);
    clip.exit_duration = std::clamp(finite_or(node, "exitDuration", 0.55), 0.0, 5.0);
    clip.enter_progress_offset = std::clamp(finite_or(node, "enterProgressOffset", 0), 0.0, 1.0);
    clip.exit_progress_offset = std::clamp(finite_or(node, "exitProgressOffset", 0), 0.0, 1.0);
    if (node.contains("focusEffect") && node.at("focusEffect").is_object()) {
        clip.focus_effect = decode_focus_effect(node.at("focusEffect"));
    }
    if (node.contains("preferredEnterDuration") && node.at("preferredEnterDuration").is_number()) {
        clip.preferred_enter_duration = node.at("preferredEnterDuration").get<double>();
    }
    if (node.contains("preferredExitDuration") && node.at("preferredExitDuration").is_number()) {
        clip.preferred_exit_duration = node.at("preferredExitDuration").get<double>();
    }
    return clip;
}

json encode_zoom_clip(const ZoomClip& clip) {
    json node;
    node["id"] = clip.id.to_string();
    node["startTime"] = clip.start_time;
    node["endTime"] = clip.end_time;
    node["scale"] = clip.scale;
    node["focus"] = encode_point(clip.focus);
    node["origin"] = raw(clip.origin);
    node["easing"] = raw(clip.easing);
    node["customCurve"] = encode_bezier(clip.custom_curve);
    node["enterDuration"] = clip.enter_duration;
    node["exitDuration"] = clip.exit_duration;
    node["enterProgressOffset"] = clip.enter_progress_offset;
    node["exitProgressOffset"] = clip.exit_progress_offset;
    if (clip.preferred_enter_duration) node["preferredEnterDuration"] = *clip.preferred_enter_duration;
    if (clip.preferred_exit_duration) node["preferredExitDuration"] = *clip.preferred_exit_duration;
    if (clip.focus_effect) node["focusEffect"] = encode_focus_effect(*clip.focus_effect);
    return node;
}


ProjectMediaReference decode_media_reference(const json& node) {
    ProjectMediaReference reference;
    reference.relative_path = node.at("relativePath").get<std::string>();
    reference.start_offset = std::max(finite_or(node, "startOffset", 0), 0.0);
    reference.source_start_time = std::max(finite_or(node, "sourceStartTime", 0), 0.0);
    const auto scale = finite_or(node, "sourceTimeScale", 0);
    if (scale > 0) reference.source_time_scale = scale;
    const auto end = finite_or(node, "sourceEndTime", -1);
    if (end >= 0) reference.source_end_time = end;
    if (node.contains("syncAnchors") && node.at("syncAnchors").is_array()) {
        for (const auto& value : node.at("syncAnchors")) {
            if (!value.is_object()) continue;
            MediaSyncAnchor anchor;
            if (const auto id = Uuid::parse(string_or(value, "id", ""))) anchor.id = *id;
            anchor.source_time = std::max(finite_or(value, "sourceTime", 0), 0.0);
            anchor.offset = std::clamp(finite_or(value, "offset", 0), -10.0, 10.0);
            reference.sync_anchors.push_back(anchor);
        }
        std::sort(reference.sync_anchors.begin(), reference.sync_anchors.end(),
            [](const auto& left, const auto& right) {
                if (left.source_time != right.source_time) return left.source_time < right.source_time;
                return left.id.to_string() < right.id.to_string();
            });
    }
    return reference;
}

json encode_media_reference(const ProjectMediaReference& reference) {
    json result{{"relativePath", reference.relative_path},
                {"startOffset", reference.start_offset},
                {"sourceStartTime", reference.source_start_time}};
    if (reference.source_time_scale) result["sourceTimeScale"] = *reference.source_time_scale;
    if (reference.source_end_time) result["sourceEndTime"] = *reference.source_end_time;
    result["syncAnchors"] = json::array();
    for (const auto& anchor : reference.sync_anchors) {
        result["syncAnchors"].push_back(json{{"id", anchor.id.to_string()},
            {"sourceTime", anchor.source_time}, {"offset", anchor.offset}});
    }
    return result;
}

}  // namespace

bool ProjectJson::decode(const std::string& text, RecorderProject& out, ProjectJsonError& error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& ex) {
        error = ProjectJsonError{ProjectJsonErrorCode::kParseFailure, "", ex.what()};
        return false;
    }
    if (!root.is_object()) {
        error = ProjectJsonError{ProjectJsonErrorCode::kNotAnObject, "", "根节点不是 JSON 对象"};
        return false;
    }

    const int version = root.contains("version") && root.at("version").is_number()
                            ? root.at("version").get<int>()
                            : 1;
    if (const auto schema_error = ProjectSchemaError::validate_for_decoding(version)) {
        error = ProjectJsonError{ProjectJsonErrorCode::kSchemaRejected, "/version",
                                 schema_error->message()};
        return false;
    }

    RecorderProject project;
    project.version = ProjectSchema::kCurrentVersion;
    project.title = string_or(root, "title", "未命名录制");
    project.created_at = string_or(root, "createdAt", "");
    if (root.contains("capture")) project.capture = decode_capture(root.at("capture"));
    if (root.contains("canvas")) project.canvas = decode_canvas(root.at("canvas"));

    if (root.contains("camera") && root.at("camera").is_object()) {
        const json& camera = root.at("camera");
        CameraStyle& style = project.camera;
        style.shape = camera_shape_from_raw(string_or(camera, "shape", "方形"));
        style.position = decode_point(camera, "position", {0.96, 0.47});
        style.size = finite_or(camera, "size", 0.29);
        style.border_width = finite_or(camera, "borderWidth", 3);
        style.shadow_strength = finite_or(camera, "shadowStrength", 0.25);
        style.roundness = finite_or(camera, "roundness", 0.5);
        style.is_mirrored = bool_or(camera, "isMirrored", true);
        style.is_hidden = bool_or(camera, "isHidden", false);
        style.scale_during_zoom = finite_or(camera, "scaleDuringZoom", 0.7);
        style.content_position = decode_point(camera, "contentPosition", {0.5, 0.5});
        style.content_scale = finite_or(camera, "contentScale", 1);
    }

    if (root.contains("audio") && root.at("audio").is_object()) {
        const json& audio = root.at("audio");
        project.audio.system_volume = std::clamp(finite_or(audio, "systemVolume", 0.82), 0.0, 1.0);
        project.audio.microphone_volume =
            std::clamp(finite_or(audio, "microphoneVolume", 0.9), 0.0, 1.0);
        project.audio.is_system_muted = bool_or(audio, "isSystemMuted", false);
        project.audio.is_microphone_muted = bool_or(audio, "isMicrophoneMuted", false);
    }

    if (root.contains("motion") && root.at("motion").is_object()) {
        const json& motion = root.at("motion");
        MotionStyle& style = project.motion;
        style.screen = screen_motion_from_raw(string_or(motion, "screen", "聚焦"));
        style.cursor = cursor_motion_from_raw(string_or(motion, "cursor", "平滑"));
        style.screen_spring_mass = finite_or(motion, "screenSpringMass", 2.4);
        style.screen_spring_stiffness = finite_or(motion, "screenSpringStiffness", 210);
        style.screen_spring_damping = finite_or(motion, "screenSpringDamping", 42);
        style.cursor_spring_mass = finite_or(motion, "cursorSpringMass", 2.8);
        style.cursor_spring_stiffness = finite_or(motion, "cursorSpringStiffness", 450);
        style.cursor_spring_damping = finite_or(motion, "cursorSpringDamping", 66);
        style.default_zoom_easing =
            easing_from_raw(string_or(motion, "defaultZoomEasing", "弹簧"));
        style.default_zoom_transition_duration =
            std::clamp(finite_or(motion, "defaultZoomTransitionDuration", 0.7), 0.0, 5.0);
        if (motion.contains("defaultZoomScale") && motion.at("defaultZoomScale").is_number()) {
            style.default_zoom_scale = motion.at("defaultZoomScale").get<double>();
        }
    }

    if (root.contains("openingSequence")) {
        project.opening_sequence = decode_opening(root.at("openingSequence"));
    }

    if (root.contains("cursorStyle") && root.at("cursorStyle").is_object()) {
        const json& cursor = root.at("cursorStyle");
        CursorStyle& style = project.cursor_style;
        const std::string asset = string_or(cursor, "assetID", "");
        if (!asset.empty()) style.asset_id = CursorAssetID{asset};
        style.size = std::clamp(finite_or(cursor, "size", 1.25), 0.25, 6.0);
        style.hide_when_idle = bool_or(cursor, "hideWhenIdle", false);
        style.idle_delay = std::clamp(finite_or(cursor, "idleDelay", 1.2), 0.2, 8.0);
        if (cursor.contains("clickEffectStyle") && cursor.at("clickEffectStyle").is_string()) {
            style.click_effect_style =
                click_effect_from_raw(cursor.at("clickEffectStyle").get<std::string>());
        } else if (cursor.contains("showsClickEffect") &&
                   cursor.at("showsClickEffect").is_boolean()) {
            style.click_effect_style = cursor.at("showsClickEffect").get<bool>()
                                           ? CursorClickEffectStyle::kRipple
                                           : CursorClickEffectStyle::kNone;
        }
        style.click_color = decode_optional_hex_color(cursor, "clickColor");
        style.click_opacity = std::clamp(finite_or(cursor, "clickOpacity", 0.8), 0.05, 1.0);
        style.click_scale = std::clamp(finite_or(cursor, "clickScale", 1.0), 0.4, 3.0);
        style.motion_tilt_strength =
            std::clamp(finite_or(cursor, "motionTiltStrength", 2.0), 2.0, 4.0);
    }

    if (root.contains("exportSettings") && root.at("exportSettings").is_object()) {
        const json& settings = root.at("exportSettings");
        if (settings.contains("frameRate") && settings.at("frameRate").is_number()) {
            project.export_settings.frame_rate = settings.at("frameRate").get<int>();
        }
        if (settings.contains("resolution") && settings.at("resolution").is_string()) {
            project.export_settings.resolution =
                canvas_resolution_from_raw(settings.at("resolution").get<std::string>())
                    .value_or(CanvasResolution::kSource);
        }
    }

    if (root.contains("timeline") && root.at("timeline").is_object()) {
        const json& timeline = root.at("timeline");
        if (timeline.contains("sourceSequence") && timeline.at("sourceSequence").is_object()) {
            const json& sequence = timeline.at("sourceSequence");
            const std::string kind = string_or(sequence, "kind", "fullRecording");
            if (kind == "edited" && sequence.contains("segments") &&
                sequence.at("segments").is_array()) {
                project.source_sequence.is_full_recording = false;
                for (const json& item : sequence.at("segments")) {
                    if (!item.is_object()) continue;
                    RecordingSegment segment;
                    if (item.contains("id") && item.at("id").is_string()) {
                        if (const auto id = Uuid::parse(item.at("id").get<std::string>())) {
                            segment.id = *id;
                        }
                    }
                    segment.source_start = std::max(finite_or(item, "sourceStart", 0), 0.0);
                    segment.source_duration = std::max(finite_or(item, "sourceDuration", 0), 0.0);
                    const double rate = finite_or(item, "playbackRate", 1);
                    segment.playback_rate = rate > 0 ? rate : 1.0;
                    project.source_sequence.segments.push_back(segment);
                }
            }
        }
        if (timeline.contains("zoomClips") && timeline.at("zoomClips").is_array()) {
            project.zoom_clips.clear();
            for (const json& item : timeline.at("zoomClips")) {
                project.zoom_clips.push_back(decode_zoom_clip(item));
            }
        }
        if (timeline.contains("primarySegmentAudioOverrides") &&
            timeline.at("primarySegmentAudioOverrides").is_object()) {
            for (auto it = timeline.at("primarySegmentAudioOverrides").begin();
                 it != timeline.at("primarySegmentAudioOverrides").end(); ++it) {
                const auto id = Uuid::parse(it.key());
                if (id) {
                    project.primary_segment_audio_overrides[*id] =
                        decode_audio_overrides(it.value());
                }
            }
        }
        if (timeline.contains("screenMotionClips") && timeline.at("screenMotionClips").is_array()) {
            for (const json& item : timeline.at("screenMotionClips")) {
                project.screen_motion_clips.push_back(decode_screen_motion_clip(item));
            }
        }
        if (timeline.contains("cameraMotionClips") && timeline.at("cameraMotionClips").is_array()) {
            for (const json& item : timeline.at("cameraMotionClips")) {
                project.camera_motion_clips.push_back(decode_camera_motion_clip(item));
            }
        }
        if (timeline.contains("mosaicClips") && timeline.at("mosaicClips").is_array()) {
            for (const json& item : timeline.at("mosaicClips")) {
                project.mosaic_clips.push_back(decode_mosaic_clip(item));
            }
        }
        if (timeline.contains("stickerClips") && timeline.at("stickerClips").is_array()) {
            for (const json& item : timeline.at("stickerClips")) {
                project.sticker_clips.push_back(decode_sticker_clip(item));
            }
        }
    }

    if (root.contains("recordingMarkers") && root.at("recordingMarkers").is_array()) {
        std::vector<Uuid> seen;
        for (const json& item : root.at("recordingMarkers")) {
            if (!item.is_object()) continue;
            RecordingMarker marker;
            if (item.contains("id") && item.at("id").is_string()) {
                if (const auto id = Uuid::parse(item.at("id").get<std::string>())) marker.id = *id;
            }
            marker.source_time = finite_or(item, "sourceTime", 0);
            marker.number = item.contains("number") && item.at("number").is_number()
                                ? item.at("number").get<int>()
                                : 0;
            if (!std::isfinite(marker.source_time) || marker.source_time < 0 ||
                marker.number <= 0) {
                continue;
            }
            if (std::find(seen.begin(), seen.end(), marker.id) != seen.end()) continue;
            seen.push_back(marker.id);
            project.recording_markers.push_back(marker);
        }
    }

    if (root.contains("media") && !root.at("media").is_null()) {
        const auto& media = root.at("media");
        if (!media.is_object() || !media.contains("screen") || !media.at("screen").is_object()) {
            error = {ProjectJsonErrorCode::kMissingRequiredField, "/media/screen",
                     "媒体清单缺少屏幕引用。"};
            return false;
        }
        ProjectMediaManifest manifest;
        for (const auto* key : {"screen", "camera", "microphone", "pointerEvents"}) {
            if (!media.contains(key) || media.at(key).is_null()) continue;
            const auto& value = media.at(key);
            if (!value.is_object() || !value.contains("relativePath") ||
                !value.at("relativePath").is_string()) {
                error = {ProjectJsonErrorCode::kMissingRequiredField,
                         std::string("/media/") + key + "/relativePath", "媒体引用缺少相对路径。"};
                return false;
            }
            auto reference = decode_media_reference(value);
            if (std::string_view(key) == "screen") manifest.screen = std::move(reference);
            else if (std::string_view(key) == "camera") manifest.camera = std::move(reference);
            else if (std::string_view(key) == "microphone") manifest.microphone = std::move(reference);
            else manifest.pointer_events = std::move(reference);
        }
        project.media = std::move(manifest);
    }

    out = std::move(project);
    error = ProjectJsonError{};
    return true;
}

std::string ProjectJson::encode(const RecorderProject& project) {
    json root;
    root["version"] = ProjectSchema::kCurrentVersion;
    root["title"] = project.title;
    root["createdAt"] = project.created_at;
    root["capture"] = encode_capture(project.capture);
    root["canvas"] = encode_canvas(project.canvas);

    json camera;
    camera["shape"] = raw(project.camera.shape);
    camera["position"] = encode_point(project.camera.position);
    camera["size"] = project.camera.size;
    camera["borderWidth"] = project.camera.border_width;
    camera["shadowStrength"] = project.camera.shadow_strength;
    camera["roundness"] = project.camera.roundness;
    camera["isMirrored"] = project.camera.is_mirrored;
    camera["isHidden"] = project.camera.is_hidden;
    camera["scaleDuringZoom"] = project.camera.scale_during_zoom;
    camera["contentPosition"] = encode_point(project.camera.content_position);
    camera["contentScale"] = project.camera.content_scale;
    root["camera"] = camera;

    root["audio"] = json{{"systemVolume", project.audio.system_volume},
                         {"microphoneVolume", project.audio.microphone_volume},
                         {"isSystemMuted", project.audio.is_system_muted},
                         {"isMicrophoneMuted", project.audio.is_microphone_muted}};

    json motion;
    motion["screen"] = raw(project.motion.screen);
    motion["cursor"] = raw(project.motion.cursor);
    motion["screenSpringMass"] = project.motion.screen_spring_mass;
    motion["screenSpringStiffness"] = project.motion.screen_spring_stiffness;
    motion["screenSpringDamping"] = project.motion.screen_spring_damping;
    motion["cursorSpringMass"] = project.motion.cursor_spring_mass;
    motion["cursorSpringStiffness"] = project.motion.cursor_spring_stiffness;
    motion["cursorSpringDamping"] = project.motion.cursor_spring_damping;
    motion["defaultZoomEasing"] = raw(project.motion.default_zoom_easing);
    motion["defaultZoomTransitionDuration"] = project.motion.default_zoom_transition_duration;
    if (project.motion.default_zoom_scale) {
        motion["defaultZoomScale"] = *project.motion.default_zoom_scale;
    }
    root["motion"] = motion;

    root["openingSequence"] = encode_opening(project.opening_sequence);

    json cursor;
    cursor["assetID"] = project.cursor_style.asset_id.raw_value;
    cursor["size"] = project.cursor_style.size;
    cursor["hideWhenIdle"] = project.cursor_style.hide_when_idle;
    cursor["idleDelay"] = project.cursor_style.idle_delay;
    cursor["showsClickEffect"] =
        project.cursor_style.click_effect_style != CursorClickEffectStyle::kNone;
    cursor["clickEffectStyle"] = raw(project.cursor_style.click_effect_style);
    if (project.cursor_style.click_color) {
        cursor["clickColor"] = project.cursor_style.click_color->value;
    }
    cursor["clickOpacity"] = project.cursor_style.click_opacity;
    cursor["clickScale"] = project.cursor_style.click_scale;
    cursor["motionTiltStrength"] = project.cursor_style.motion_tilt_strength;
    root["cursorStyle"] = cursor;

    root["exportSettings"] = json{{"frameRate", project.export_settings.frame_rate},
                                  {"resolution", to_raw_value(project.export_settings.resolution)}};

    json timeline;
    if (project.source_sequence.is_full_recording) {
        timeline["sourceSequence"] = json{{"kind", "fullRecording"}};
    } else {
        json segments = json::array();
        for (const RecordingSegment& segment : project.source_sequence.segments) {
            segments.push_back(json{{"id", segment.id.to_string()},
                                    {"sourceStart", segment.source_start},
                                    {"sourceDuration", segment.source_duration},
                                    {"playbackRate", segment.playback_rate}});
        }
        timeline["sourceSequence"] = json{{"kind", "edited"}, {"segments", segments}};
    }
    json zooms = json::array();
    for (const ZoomClip& clip : project.zoom_clips) zooms.push_back(encode_zoom_clip(clip));
    timeline["zoomClips"] = zooms;
    json audio_overrides = json::object();
    for (const auto& [id, overrides] : project.primary_segment_audio_overrides) {
        audio_overrides[id.to_string()] = encode_audio_overrides(overrides);
    }
    timeline["primarySegmentAudioOverrides"] = std::move(audio_overrides);

    json screen_motion = json::array();
    for (const ScreenMotionClip& clip : project.screen_motion_clips) {
        screen_motion.push_back(encode_screen_motion_clip(clip));
    }
    timeline["screenMotionClips"] = std::move(screen_motion);

    json camera_motion = json::array();
    for (const CameraMotionClip& clip : project.camera_motion_clips) {
        camera_motion.push_back(encode_camera_motion_clip(clip));
    }
    timeline["cameraMotionClips"] = std::move(camera_motion);

    json mosaics = json::array();
    for (const MosaicClip& clip : project.mosaic_clips) mosaics.push_back(encode_mosaic_clip(clip));
    timeline["mosaicClips"] = std::move(mosaics);

    json stickers = json::array();
    for (const StickerClip& clip : project.sticker_clips) stickers.push_back(encode_sticker_clip(clip));
    timeline["stickerClips"] = std::move(stickers);
    root["timeline"] = timeline;

    json markers = json::array();
    for (const RecordingMarker& marker : project.recording_markers) {
        markers.push_back(json{{"id", marker.id.to_string()},
                               {"sourceTime", marker.source_time},
                               {"number", marker.number}});
    }
    root["recordingMarkers"] = markers;

    if (project.media) {
        const auto& media = *project.media;
        json manifest{{"screen", encode_media_reference(media.screen)}};
        if (media.camera) manifest["camera"] = encode_media_reference(*media.camera);
        if (media.microphone) manifest["microphone"] = encode_media_reference(*media.microphone);
        if (media.pointer_events) manifest["pointerEvents"] = encode_media_reference(*media.pointer_events);
        root["media"] = std::move(manifest);
    }

    return root.dump(2);
}


std::optional<RecorderProject> decode_project(const std::string& text) {
    RecorderProject project;
    ProjectJsonError error;
    if (!ProjectJson::decode(text, project, error)) return std::nullopt;
    return project;
}

}  // namespace dogsc
