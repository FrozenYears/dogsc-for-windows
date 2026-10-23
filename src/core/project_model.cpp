#include "dogsc/project_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dogsc {

std::string ProjectSchemaError::message() const {
    char buffer[256];
    switch (code) {
        case ProjectSchemaErrorCode::kUnsupportedLegacyVersion:
            std::snprintf(buffer, sizeof(buffer),
                          "项目版本 %d 过旧；当前应用最低支持版本 %d。", found, bound);
            break;
        case ProjectSchemaErrorCode::kUnsupportedFutureVersion:
            std::snprintf(buffer, sizeof(buffer),
                          "项目版本 %d 来自更新的应用；当前应用只支持到版本 %d，为避免损坏项目已停止打开。",
                          found, bound);
            break;
        case ProjectSchemaErrorCode::kRefusingToEncodeUnsupportedVersion:
            std::snprintf(buffer, sizeof(buffer), "拒绝写入项目版本 %d；可写入版本下限为 %d。",
                          found, bound);
            break;
        case ProjectSchemaErrorCode::kRefusingToEncodeFutureVersion:
            std::snprintf(buffer, sizeof(buffer), "拒绝用当前应用覆盖未来项目版本 %d；当前应用只支持到版本 %d。",
                          found, bound);
            break;
    }
    return buffer;
}

std::optional<ProjectSchemaError> ProjectSchemaError::validate_for_decoding(int version) {
    if (version < ProjectSchema::kMinimumSupportedVersion) {
        return ProjectSchemaError{ProjectSchemaErrorCode::kUnsupportedLegacyVersion, version,
                                  ProjectSchema::kMinimumSupportedVersion};
    }
    if (version > ProjectSchema::kCurrentVersion) {
        return ProjectSchemaError{ProjectSchemaErrorCode::kUnsupportedFutureVersion, version,
                                  ProjectSchema::kCurrentVersion};
    }
    return std::nullopt;
}

std::optional<ProjectSchemaError> ProjectSchemaError::validate_for_encoding(int version) {
    if (version < ProjectSchema::kMinimumSupportedVersion) {
        return ProjectSchemaError{ProjectSchemaErrorCode::kRefusingToEncodeUnsupportedVersion,
                                  version, ProjectSchema::kMinimumSupportedVersion};
    }
    if (version > ProjectSchema::kCurrentVersion) {
        return ProjectSchemaError{ProjectSchemaErrorCode::kRefusingToEncodeFutureVersion, version,
                                  ProjectSchema::kCurrentVersion};
    }
    return std::nullopt;
}

std::string to_raw_value(CanvasAspectRatio value) {
    switch (value) {
        case CanvasAspectRatio::kAdaptive:
            return "适应";
        case CanvasAspectRatio::kLandscape:
            return "16:9";
        case CanvasAspectRatio::kStandard:
            return "4:3";
        case CanvasAspectRatio::kPortrait:
            return "9:16";
        case CanvasAspectRatio::kSquare:
            return "1:1";
        case CanvasAspectRatio::kStandardPortrait:
            return "3:4";
        case CanvasAspectRatio::kCinema:
            return "2.39:1";
        case CanvasAspectRatio::kCinemaPortrait:
            return "1:2.39";
        case CanvasAspectRatio::kCustom:
            return "自定义";
    }
    return "适应";
}

std::optional<CanvasAspectRatio> canvas_aspect_ratio_from_raw(const std::string& raw) {
    static constexpr CanvasAspectRatio kAll[] = {
        CanvasAspectRatio::kAdaptive,     CanvasAspectRatio::kLandscape,
        CanvasAspectRatio::kStandard,     CanvasAspectRatio::kPortrait,
        CanvasAspectRatio::kSquare,       CanvasAspectRatio::kStandardPortrait,
        CanvasAspectRatio::kCinema,       CanvasAspectRatio::kCinemaPortrait,
        CanvasAspectRatio::kCustom,
    };
    for (CanvasAspectRatio value : kAll) {
        if (to_raw_value(value) == raw) return value;
    }
    return std::nullopt;
}

std::string to_raw_value(CanvasResolution value) {
    switch (value) {
        case CanvasResolution::kSource:
            return "原始素材";
        case CanvasResolution::kFullHD:
            return "1080p";
        case CanvasResolution::kQuadHD:
            return "1440p";
        case CanvasResolution::kUltraHD:
            return "4K";
    }
    return "原始素材";
}

std::optional<CanvasResolution> canvas_resolution_from_raw(const std::string& raw) {
    static constexpr CanvasResolution kAll[] = {
        CanvasResolution::kSource,
        CanvasResolution::kFullHD,
        CanvasResolution::kQuadHD,
        CanvasResolution::kUltraHD,
    };
    for (CanvasResolution value : kAll) {
        if (to_raw_value(value) == raw) return value;
    }
    return std::nullopt;
}

BackgroundSource BackgroundSource::safe_fallback() {
    BackgroundSource source;
    source.kind = BackgroundSourceKind::kPattern;
    source.pattern_preset = "黑曜石网格";
    return source;
}

namespace {

/// 预设短边/长边，与上游 `CanvasResolution.presetEdges` 一致。
struct PresetEdges {
    int short_edge = 0;
    int long_edge = 0;
};

std::optional<PresetEdges> preset_edges(CanvasResolution resolution) {
    switch (resolution) {
        case CanvasResolution::kSource:
            return std::nullopt;
        case CanvasResolution::kFullHD:
            return PresetEdges{1080, 1920};
        case CanvasResolution::kQuadHD:
            return PresetEdges{1440, 2560};
        case CanvasResolution::kUltraHD:
            return PresetEdges{2160, 3840};
    }
    return std::nullopt;
}

/// 向上游看齐的偶数化：四舍五入到最近的偶数，且不小于 2。
int even_from_double(double value) {
    const double rounded = std::round(value / 2) * 2;
    return std::max(static_cast<int>(rounded), 2);
}

std::optional<double> fixed_aspect_ratio(const CanvasStyle& style) {
    switch (style.aspect_ratio) {
        case CanvasAspectRatio::kAdaptive:
            return std::nullopt;
        case CanvasAspectRatio::kLandscape:
            return 16.0 / 9.0;
        case CanvasAspectRatio::kStandard:
            return 4.0 / 3.0;
        case CanvasAspectRatio::kPortrait:
            return 9.0 / 16.0;
        case CanvasAspectRatio::kSquare:
            return 1.0;
        case CanvasAspectRatio::kStandardPortrait:
            return 3.0 / 4.0;
        case CanvasAspectRatio::kCinema:
            return 2.39;
        case CanvasAspectRatio::kCinemaPortrait:
            return 1.0 / 2.39;
        case CanvasAspectRatio::kCustom:
            break;
    }
    if (!(style.custom_aspect_ratio.height > 0) ||
        !std::isfinite(style.custom_aspect_ratio.width) ||
        !std::isfinite(style.custom_aspect_ratio.height)) {
        return 1.0;
    }
    return style.custom_aspect_ratio.width / style.custom_aspect_ratio.height;
}

CanvasDimensions source_pixel_dimensions(const CanvasStyle& style,
                                         const std::optional<CanvasDimensions>& source_pixel_size) {
    const CanvasDimensions source = source_pixel_size.value_or(CanvasDimensions{});
    const NormalizedCrop crop = style.crop.clamped();
    const double cropped_width = std::max(static_cast<double>(source.width) * crop.width, 2.0);
    const double cropped_height = std::max(static_cast<double>(source.height) * crop.height, 2.0);

    if (style.aspect_ratio == CanvasAspectRatio::kAdaptive) {
        return CanvasDimensions{even_from_double(cropped_width), even_from_double(cropped_height)};
    }

    const double target_ratio = fixed_aspect_ratio(style).value_or(cropped_width / cropped_height);
    if (cropped_width / cropped_height >= target_ratio) {
        return CanvasDimensions{even_from_double(cropped_height * target_ratio),
                                even_from_double(cropped_height)};
    }
    return CanvasDimensions{even_from_double(cropped_width),
                            even_from_double(cropped_width / target_ratio)};
}

}  // namespace

CanvasDimensions CanvasStyle::pixel_dimensions(
    CanvasResolution resolution, std::optional<double> source_aspect_ratio,
    std::optional<CanvasDimensions> source_pixel_size) const {
    const std::optional<PresetEdges> preset = preset_edges(resolution);
    if (!preset.has_value()) {
        return source_pixel_dimensions(*this, source_pixel_size);
    }

    const int short_edge = preset->short_edge;
    switch (aspect_ratio) {
        case CanvasAspectRatio::kAdaptive: {
            const double ratio = std::clamp(source_aspect_ratio.value_or(16.0 / 9.0), 0.2, 5.0);
            const int long_edge = preset->long_edge;
            if (ratio >= 1) {
                return CanvasDimensions{even_from_double(long_edge),
                                        even_from_double(static_cast<double>(long_edge) / ratio)};
            }
            return CanvasDimensions{even_from_double(static_cast<double>(long_edge) * ratio),
                                    even_from_double(long_edge)};
        }
        case CanvasAspectRatio::kLandscape:
            return CanvasDimensions{even_from_double(static_cast<double>(short_edge) * 16 / 9),
                                    even_from_double(short_edge)};
        case CanvasAspectRatio::kStandard:
            return CanvasDimensions{even_from_double(static_cast<double>(short_edge) * 4 / 3),
                                    even_from_double(short_edge)};
        case CanvasAspectRatio::kPortrait:
            return CanvasDimensions{even_from_double(short_edge),
                                    even_from_double(static_cast<double>(short_edge) * 16 / 9)};
        case CanvasAspectRatio::kSquare:
            return CanvasDimensions{even_from_double(short_edge), even_from_double(short_edge)};
        case CanvasAspectRatio::kStandardPortrait:
            return CanvasDimensions{even_from_double(short_edge),
                                    even_from_double(static_cast<double>(short_edge) * 4 / 3)};
        case CanvasAspectRatio::kCinema:
        case CanvasAspectRatio::kCinemaPortrait:
        case CanvasAspectRatio::kCustom: {
            const double ratio = fixed_aspect_ratio(*this).value_or(1.0);
            const double long_edge = static_cast<double>(preset->long_edge);
            if (ratio >= 1) {
                return CanvasDimensions{even_from_double(long_edge),
                                        even_from_double(long_edge / ratio)};
            }
            return CanvasDimensions{even_from_double(long_edge * ratio), even_from_double(long_edge)};
        }
    }
    return CanvasDimensions{};
}

}  // namespace dogsc
