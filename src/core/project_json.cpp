#include "dogsc/project_json.hpp"

#include <algorithm>
#include <cmath>
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
