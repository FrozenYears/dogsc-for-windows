#include "commands.hpp"

#include <array>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>
#include <winrt/base.h>
#include <limits>
#include <objbase.h>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <string_view>
#include <windows.h>

#include "dogsc/project_json.hpp"
#include "dogsc/project_timeline_editing.hpp"
#include "dogsc/timeline_map.hpp"

namespace dogsc::cli {
namespace {
using json = nlohmann::ordered_json;

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("无法打开工程文件: " + path.string());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::filesystem::path normalize_project_path(const std::filesystem::path& value) {
    std::error_code error;
    if (std::filesystem::is_directory(value, error)) return value / "project.json";
    if (value.filename() == "project.json" || value.extension() == ".json") return value;
    throw std::invalid_argument("项目路径必须是 project.json 或包含 project.json 的工程目录");
}

RecorderProject read_project(const std::filesystem::path& path) {
    RecorderProject project;
    ProjectJsonError error;
    if (!ProjectJson::decode(read_text(path), project, error)) {
        throw std::runtime_error("工程解析失败 " + error.path + ": " + error.detail);
    }
    return project;
}

TimeInterval full_source_duration(const RecorderProject& project) {
    if (!project.media || !project.media->screen.source_end_time) {
        throw std::runtime_error("工程缺少屏幕媒体时长，无法编辑时间线");
    }
    const auto duration = *project.media->screen.source_end_time - project.media->screen.source_start_time;
    if (!std::isfinite(duration) || duration <= 0) {
        throw std::runtime_error("屏幕媒体时长无效");
    }
    return duration;
}

Uuid new_uuid(const RecorderProject& project) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("无法生成新的片段 ID");
    Uuid result;
    std::memcpy(result.bytes.data(), &guid, sizeof(guid));
    result.bytes[6] = static_cast<std::uint8_t>((result.bytes[6] & 0x0f) | 0x40);
    result.bytes[8] = static_cast<std::uint8_t>((result.bytes[8] & 0x3f) | 0x80);
    for (const auto& segment : project.source_sequence.segments) {
        if (segment.id == result) return new_uuid(project);
    }
    return result;
}

std::wstring required_argument(int& index, int count, wchar_t** arguments, const char* name) {
    if (++index >= count) throw std::invalid_argument(std::string("项目命令缺少 ") + name);
    return arguments[index];
}

double parse_double(const std::wstring& value, const char* name) {
    std::size_t consumed = 0;
    const double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result)) {
        throw std::invalid_argument(std::string(name) + " 必须是有限数字");
    }
    return result;
}

int parse_int(const std::wstring& value, const char* name) {
    std::size_t consumed = 0;
    const int result = std::stoi(value, &consumed);
    if (consumed != value.size()) throw std::invalid_argument(std::string(name) + " 必须是整数");
    return result;
}

std::string edit_error_message(const ProjectTimelineEditingError& error) {
    switch (error.code) {
        case ProjectTimelineEditingErrorCode::kSegmentNotFound: return "找不到主时间线片段";
        case ProjectTimelineEditingErrorCode::kSplitOutsideSegment: return "分割点不在片段内部";
        case ProjectTimelineEditingErrorCode::kTrimOutsideSegment: return "裁切点超出片段范围";
        case ProjectTimelineEditingErrorCode::kCannotRemoveFinalSegment: return "不能删除最后一个主时间线片段";
        case ProjectTimelineEditingErrorCode::kInvalidSourceSequence: return "主时间线片段序列无效";
        case ProjectTimelineEditingErrorCode::kInvalidClip: return "工程中存在无效叠加片段";
        case ProjectTimelineEditingErrorCode::kOverlappingClips: return "工程中的叠加片段发生重叠";
        default: return "时间线编辑失败，错误码 " + std::to_string(static_cast<int>(error.code));
    }
}

json snapshot(const RecorderProject& project, TimeInterval full_duration) {
    TimelineMap map;
    if (const auto error = TimelineMap::create(project.source_sequence, full_duration, map)) {
        throw std::runtime_error(std::string("时间线解析失败: ") + error->message());
    }
    json result;
    result["project"] = json::parse(ProjectJson::encode(project));
    result["fullSourceDuration"] = full_duration;
    result["outputDuration"] = map.output_duration();
    result["segments"] = json::array();
    for (const auto& segment : map.segments()) {
        result["segments"].push_back({
            {"id", segment.id.to_string()},
            {"sourceStart", segment.source_start},
            {"sourceDuration", segment.source_duration},
            {"playbackRate", segment.playback_rate},
            {"outputStart", segment.output_start},
            {"outputEnd", segment.output_end()},
        });
    }
    result["clipCounts"] = {
        {"zoom", project.zoom_clips.size()},
        {"screenMotion", project.screen_motion_clips.size()},
        {"cameraMotion", project.camera_motion_clips.size()},
        {"mosaic", project.mosaic_clips.size()},
        {"sticker", project.sticker_clips.size()},
    };
    return result;
 }

std::filesystem::path history_directory(const std::filesystem::path& path) {
    return path.parent_path() / L".project-history";
}
std::filesystem::path draft_path(const std::filesystem::path& path) {
    return path.parent_path() / L".project-draft.json";
}
std::filesystem::path recovery_path(const std::filesystem::path& path) {
    return path.parent_path() / L".project-recovery.json";
}

struct HistoryState {
    std::vector<std::string> states;
    std::size_t current = 0;
};

void copy_file_atomically(const std::filesystem::path& source,
                          const std::filesystem::path& target) {
    const auto temporary = target.parent_path() / (target.filename().wstring() + L".dogsc-copy.tmp");
    std::error_code error;
    std::filesystem::remove(temporary, error);
    if (!std::filesystem::copy_file(source, temporary,
            std::filesystem::copy_options::overwrite_existing, error)) {
        throw std::runtime_error("工程版本复制失败");
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("工程版本原子替换失败");
    }
}

std::string history_state_name(std::size_t index) {
    std::ostringstream name;
    name << "state-" << std::setw(6) << std::setfill('0') << index << ".json";
    return name.str();
}

HistoryState read_history(const std::filesystem::path& path) {
    const auto manifest = history_directory(path) / L"manifest.json";
    if (!std::filesystem::exists(manifest)) return {};
    const auto root = json::parse(read_text(manifest));
    HistoryState history;
    if (root.contains("states") && root.at("states").is_array()) {
        for (const auto& item : root.at("states")) {
            if (item.is_string()) history.states.push_back(item.get<std::string>());
        }
    }
    if (history.states.empty()) return {};
    history.current = root.value("current", 0U);
    if (history.current >= history.states.size()) {
        throw std::runtime_error("工程历史索引损坏");
    }
    return history;
}

void write_text_atomically(const std::filesystem::path& path, const std::string& text,
                           const wchar_t* suffix) {
    const auto temporary = path.parent_path() / (path.filename().wstring() + suffix);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::remove(temporary, error);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("无法创建工程临时文件");
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            std::filesystem::remove(temporary, error);
            throw std::runtime_error("工程临时文件写入失败");
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("工程文件原子替换失败");
    }
}

void write_recovery_manifest(const std::filesystem::path& path) {
    json manifest{{"version", 1}, {"projectPath", path.string()},
                  {"draftPath", draft_path(path).string()}};
    write_text_atomically(recovery_path(path), manifest.dump(2), L".dogsc-recovery.tmp");
}

void write_project_file(const std::filesystem::path& path, const std::string& text) {
    const auto previous = path.parent_path() / L"project.previous.json";
    std::error_code error;
    if (!std::filesystem::copy_file(path, previous,
            std::filesystem::copy_options::overwrite_existing, error)) {
        throw std::runtime_error("无法保留 project.previous.json");
    }
    write_text_atomically(path, text, L".dogsc-edit.tmp");
}

void save_history(const std::filesystem::path& path, const HistoryState& history) {
    json manifest{{"current", history.current}, {"states", history.states}};
    write_text_atomically(history_directory(path) / L"manifest.json", manifest.dump(2),
                          L".dogsc-history.tmp");
}

void write_history_state(const std::filesystem::path& path, const std::string& name,
                         const std::string& text) {
    write_text_atomically(history_directory(path) / std::filesystem::path(name), text,
                          L".dogsc-state.tmp");
}

void commit_project(const std::filesystem::path& path, const RecorderProject& project) {
    const auto text = ProjectJson::encode(project);
    auto history = read_history(path);
    if (history.states.empty()) {
        history.states.push_back(history_state_name(0));
        history.current = 0;
        write_history_state(path, history.states.front(), read_text(path));
    }
    std::error_code error;
    while (history.states.size() > history.current + 1) {
        std::filesystem::remove(history_directory(path) / history.states.back(), error);
        history.states.pop_back();
    }
    const auto next = history_state_name(history.states.size());
    write_history_state(path, next, text);
    write_project_file(path, text);
    history.states.push_back(next);
    history.current = history.states.size() - 1;
    save_history(path, history);
}

void change_revision(const std::filesystem::path& path, bool redo) {
    auto history = read_history(path);
    if (history.states.empty()) throw std::runtime_error("工程尚未建立撤销历史");
    if (!redo && history.current == 0) throw std::runtime_error("没有可撤销的工程版本");
    if (redo && history.current + 1 >= history.states.size()) {
        throw std::runtime_error("没有可重做的工程版本");
    }
    const auto target = redo ? history.current + 1 : history.current - 1;
    const auto target_path = history_directory(path) / history.states[target];
    const auto previous = path.parent_path() / L"project.previous.json";
    std::error_code error;
    if (!std::filesystem::copy_file(path, previous,
            std::filesystem::copy_options::overwrite_existing, error)) {
        throw std::runtime_error("无法保留历史切换前的工程版本");
    }
    copy_file_atomically(target_path, path);
    history.current = target;
    save_history(path, history);
}

bool draft_is_modified(const std::filesystem::path& path) {
    const auto draft = draft_path(path);
    if (!std::filesystem::exists(draft)) return false;
    try {
        const auto committed = nlohmann::json::parse(read_text(path));
        const auto pending = nlohmann::json::parse(read_text(draft));
        return committed != pending;
    } catch (...) {
        return true;
    }
}

void attach_history(json& result, const std::filesystem::path& path) {
    const auto history = read_history(path);
    result["history"] = {
        {"canUndo", !history.states.empty() && history.current > 0},
        {"canRedo", !history.states.empty() && history.current + 1 < history.states.size()},
    };
    const bool dirty = draft_is_modified(path);
    result["session"] = {
        {"dirty", dirty},
        {"hasRecovery", dirty},
    };
}

json list_projects(const std::filesystem::path& root) {
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) throw std::runtime_error("项目库目录不存在");
    json result{{"root", root.string()}, {"projects", json::array()}};
    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (!error && entry.is_directory(error)) entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.path().filename().string() < right.path().filename().string();
    });
    for (const auto& entry : entries) {
        const auto project_path = entry.path() / "project.json";
        if (!std::filesystem::is_regular_file(project_path, error)) continue;
        json item{{"name", entry.path().filename().string()}, {"path", entry.path().string()}};
        try {
            const auto project = read_project(project_path);
            item["title"] = project.title;
            item["createdAt"] = project.created_at;
            try { item["duration"] = full_source_duration(project); }
            catch (const std::exception& exception) { item["error"] = exception.what(); }
        } catch (const std::exception& exception) {
            item["error"] = exception.what();
        }
        result["projects"].push_back(std::move(item));
    }
    return result;
}

json rename_project(const std::filesystem::path& project_path, std::wstring_view requested_name) {
    if (requested_name.empty() || requested_name == L"." || requested_name == L"..") {
        throw std::invalid_argument("工程名称不能为空");
    }
    if (requested_name.back() == L'.' || requested_name.back() == L' ') {
        throw std::invalid_argument("工程名称不能以句点或空格结尾");
    }
    constexpr std::wstring_view invalid = L"<>:\"/\\|?*";
    if (requested_name.find_first_of(invalid) != std::wstring_view::npos ||
        requested_name.find(L"..") != std::wstring_view::npos) {
        throw std::invalid_argument("工程名称包含 Windows 不允许的字符");
    }
    const auto source = normalize_project_path(project_path).parent_path();
    const auto target = source.parent_path() / std::wstring(requested_name);
    std::error_code error;
    if (std::filesystem::exists(target, error)) throw std::runtime_error("目标工程目录已存在");
    std::filesystem::rename(source, target, error);
    if (error) throw std::runtime_error("工程重命名失败: " + error.message());
    return json{{"name", target.filename().string()}, {"path", target.string()}};
}
void ensure_draft(const std::filesystem::path& path) {
    const auto draft = draft_path(path);
    if (!std::filesystem::exists(draft)) copy_file_atomically(path, draft);
}

RecorderProject read_draft(const std::filesystem::path& path) {
    const auto draft = draft_path(path);
    if (!std::filesystem::exists(draft)) throw std::runtime_error("工程没有未保存草稿");
    return read_project(draft);
}

void write_draft(const std::filesystem::path& path, const RecorderProject& project) {
    write_text_atomically(draft_path(path), ProjectJson::encode(project), L".dogsc-draft.tmp");
    if (draft_is_modified(path)) {
        write_recovery_manifest(path);
    } else {
        std::error_code error;
        std::filesystem::remove(recovery_path(path), error);
    }
}
RecorderProject apply_style(const RecorderProject& project, std::wstring_view style,
                            int count, wchar_t** arguments) {
    if (count != 1) throw std::invalid_argument("style 需要一个值");
    auto output = project;
    if (style == L"background") {
        const std::string preset = winrt::to_string(arguments[0]);
        static constexpr std::array<std::string_view, 5> presets{
            "黑曜石网格", "工程白网格", "暗夜星阵", "建筑极简点", "立体等角网"};
        if (std::find(presets.begin(), presets.end(), preset) == presets.end()) {
            throw std::invalid_argument("背景预设无效");
        }
        output.canvas.background_source.pattern_preset = preset;
    } else if (style == L"screen-frame") {
        const auto value = winrt::to_string(arguments[0]);
        static constexpr std::array<std::string_view, 10> frames{
            "none", "windowLight", "windowDark", "browserLight", "browserDark",
            "devicePhone", "deviceTablet", "devicePhonePortrait", "devicePhoneLandscape", "deviceTabletPortrait"};
        if (std::find(frames.begin(), frames.end(), value) == frames.end() && value != "deviceTabletLandscape") {
            throw std::invalid_argument("screen-frame 无效");
        }
        output.canvas.screen_frame = value;
    } else if (style == L"screen-motion") {
        const auto value = std::wstring_view(arguments[0]);
        if (value == L"聚焦") output.motion.screen = ScreenMotionStyle::kFocused;
        else if (value == L"平滑") output.motion.screen = ScreenMotionStyle::kSmooth;
        else throw std::invalid_argument("screen-motion 需要 聚焦/平滑");
    } else if (style == L"cursor-motion") {
        const auto value = std::wstring_view(arguments[0]);
        if (value == L"平滑") output.motion.cursor = CursorMotionStyle::kSmooth;
        else if (value == L"适中") output.motion.cursor = CursorMotionStyle::kMedium;
        else if (value == L"快速") output.motion.cursor = CursorMotionStyle::kRapid;
        else if (value == L"无") output.motion.cursor = CursorMotionStyle::kNone;
        else throw std::invalid_argument("cursor-motion 无效");
    } else if (style == L"opening-enabled") {
        const auto value = std::wstring_view(arguments[0]);
        if (value != L"true" && value != L"false") throw std::invalid_argument("opening-enabled 需要 true/false");
        output.opening_sequence.is_enabled = value == L"true";
    } else if (style == L"opening-preset") {
        const auto value = std::wstring_view(arguments[0]);
        if (value == L"聚拢") output.opening_sequence.preset = OpeningSequencePreset::kConverge;
        else if (value == L"侧滑") output.opening_sequence.preset = OpeningSequencePreset::kSideSlide;
        else if (value == L"轻 3D") output.opening_sequence.preset = OpeningSequencePreset::kLight3D;
        else throw std::invalid_argument("opening-preset 无效");
    } else if (style == L"opening-duration") {
        output.opening_sequence.duration = std::clamp(parse_double(arguments[0], "opening-duration"), 0.18, 10.0);
    } else if (style == L"padding") {
        output.canvas.padding = std::clamp(parse_double(arguments[0], "padding"), 0.0, 1000.0);
    } else if (style == L"camera-hidden") {
        const auto value = std::wstring_view(arguments[0]);
        if (value != L"true" && value != L"false") throw std::invalid_argument("camera-hidden 需要 true/false");
        output.camera.is_hidden = value == L"true";
    } else if (style == L"camera-shape") {
        const auto value = winrt::to_string(arguments[0]);
        if (value == "方形") output.camera.shape = CameraShape::kSquare;
        else if (value == "横向") output.camera.shape = CameraShape::kHorizontal;
        else if (value == "纵向") output.camera.shape = CameraShape::kVertical;
        else if (value == "圆形") output.camera.shape = CameraShape::kCircle;
        else if (value == "原始比例") output.camera.shape = CameraShape::kOriginal;
        else throw std::invalid_argument("camera-shape 无效");
    } else if (style == L"camera-size") {
        output.camera.size = std::clamp(parse_double(arguments[0], "camera-size"), 0.05, 1.0);
        output.camera.is_hidden = false;
    } else if (style == L"cursor-size") {
        output.cursor_style.size = std::clamp(parse_double(arguments[0], "cursor-size"), 0.25, 6.0);
    } else if (style == L"cursor-effect") {
        const auto value = std::wstring_view(arguments[0]);
        if (value == L"ripple") output.cursor_style.click_effect_style = CursorClickEffectStyle::kRipple;
        else if (value == L"glow") output.cursor_style.click_effect_style = CursorClickEffectStyle::kGlow;
        else if (value == L"pulse") output.cursor_style.click_effect_style = CursorClickEffectStyle::kPulse;
        else if (value == L"burst") output.cursor_style.click_effect_style = CursorClickEffectStyle::kBurst;
        else if (value == L"none") output.cursor_style.click_effect_style = CursorClickEffectStyle::kNone;
        else throw std::invalid_argument("cursor-effect 无效");
    } else if (style == L"cursor-opacity") {
        output.cursor_style.click_opacity = std::clamp(parse_double(arguments[0], "cursor-opacity"), 0.05, 1.0);
    } else if (style == L"cursor-scale") {
        output.cursor_style.click_scale = std::clamp(parse_double(arguments[0], "cursor-scale"), 0.4, 3.0);
    } else {
        throw std::invalid_argument("未知 style 操作");
    }
    return output;
}

RecorderProject apply_edit(const RecorderProject& project, std::wstring_view operation,
                           int count, wchar_t** arguments);
RecorderProject apply_project_operation(const RecorderProject& project, std::wstring_view operation,
                                        int count, wchar_t** arguments) {
    if (operation == L"style") {
        if (count < 1) throw std::invalid_argument("style 缺少子操作");
        return apply_style(project, arguments[0], count - 1, arguments + 1);
    }
    return apply_edit(project, operation, count, arguments);
}

RecorderProject apply_edit(const RecorderProject& project, std::wstring_view operation,
                           int count, wchar_t** arguments) {
    const auto duration = full_source_duration(project);
    if (count < 1) throw std::invalid_argument("项目 edit 缺少操作参数");
    auto result = ProjectTimelineEditResult{};
    if (operation == L"marker") {
        if (count != 1) throw std::invalid_argument("marker 需要 source-time");
        auto output = project;
        RecordingMarker marker;
        marker.id = new_uuid(project);
        marker.source_time = std::clamp(parse_double(arguments[0], "source-time"), 0.0, duration);
        marker.number = 1;
        for (const auto& existing : output.recording_markers) marker.number = std::max(marker.number, existing.number + 1);
        output.recording_markers.push_back(marker);
        result = std::move(output);
    } else if (operation == L"split") {
        if (count != 1) throw std::invalid_argument("split 需要 output-time");
        result = ProjectTimelineEditing::split_primary_segment(
            project, parse_double(arguments[0], "output-time"), new_uuid(project), duration);
    } else if (operation == L"zoom") {
        if (count != 7) throw std::invalid_argument("zoom 需要 start、duration、focus-x、focus-y、scale、enter、exit");
        ZoomClip clip;
        clip.id = new_uuid(project);
        clip.start_time = parse_double(arguments[0], "start");
        clip.end_time = clip.start_time + std::max(parse_double(arguments[1], "duration"), 0.01);
        clip.focus = {parse_double(arguments[2], "focus-x"), parse_double(arguments[3], "focus-y")};
        clip.scale = std::clamp(parse_double(arguments[4], "scale"), 1.0, 8.0);
        clip.enter_duration = std::clamp(parse_double(arguments[5], "enter"), 0.0, clip.duration());
        clip.exit_duration = std::clamp(parse_double(arguments[6], "exit"), 0.0, clip.duration());
        clip.origin = ZoomKeyframeOrigin::kManual;
        clip.easing = ZoomEasingPreset::kCubic;
        result = ProjectTimelineEditing::inserting_zoom(project, clip);
    } else if (operation == L"sticker") {
        if (count != 6) throw std::invalid_argument("sticker 需要 start、duration、relative-path、x、y、width");
        StickerClip clip;
        clip.id = new_uuid(project);
        clip.timing = {parse_double(arguments[0], "start"), parse_double(arguments[1], "duration")};
        clip.relative_path = winrt::to_string(arguments[2]);
        clip.position = {parse_double(arguments[3], "x"), parse_double(arguments[4], "y")};
        clip.width = parse_double(arguments[5], "width");
        result = ProjectTimelineEditing::inserting_sticker(project, clip);
    } else if (operation == L"screen-motion") {
        if (count != 9) throw std::invalid_argument("screen-motion 需要 start、duration、x、y、scale、rotation-x、rotation-y、rotation-z、perspective");
        ScreenMotionClip clip;
        clip.id = new_uuid(project);
        clip.timing.start_time = parse_double(arguments[0], "start");
        clip.timing.duration = std::max(parse_double(arguments[1], "duration"), 0.01);
        clip.timing.lead_in_duration = std::min(0.7, clip.timing.duration);
        clip.target.position = {parse_double(arguments[2], "x"), parse_double(arguments[3], "y")};
        clip.target.scale = std::clamp(parse_double(arguments[4], "scale"), 0.2, 4.0);
        clip.target.rotation_x = std::clamp(parse_double(arguments[5], "rotation-x"), -89.0, 89.0);
        clip.target.rotation_y = std::clamp(parse_double(arguments[6], "rotation-y"), -89.0, 89.0);
        clip.target.rotation_z = std::clamp(parse_double(arguments[7], "rotation-z"), -180.0, 180.0);
        clip.target.perspective = std::clamp(parse_double(arguments[8], "perspective"), 0.0, 1.0);
        result = ProjectTimelineEditing::inserting_screen_motion(project, clip);
    } else {
        const bool remove = operation == L"remove";
        if ((remove && count != 1) || (!remove && count < 2)) {
            throw std::invalid_argument(remove ? "remove 需要 segment-id" : "该操作需要 segment-id 和参数");
        }
        const auto id = Uuid::parse(winrt::to_string(arguments[0]));
        if (!id) throw std::invalid_argument("segment-id 不是有效 UUID");
        if (operation == L"trim") {
            if (count != 3 || (std::wstring_view(arguments[1]) != L"left" &&
                               std::wstring_view(arguments[1]) != L"right")) {
                throw std::invalid_argument("trim 需要 segment-id、left|right、output-time");
            }
            const auto edge = std::wstring_view(arguments[1]) == L"left"
                ? RecordingSegmentTrimEdge::kLeft : RecordingSegmentTrimEdge::kRight;
            result = ProjectTimelineEditing::trim_primary_segment(
                project, *id, edge, parse_double(arguments[2], "output-time"), duration);
        } else if (operation == L"remove") {
            result = ProjectTimelineEditing::remove_primary_segment(project, *id, duration);
        } else if (operation == L"move") {
            if (count != 2) throw std::invalid_argument("move 需要 segment-id 和 index");
            result = ProjectTimelineEditing::move_primary_segment(
                project, *id, parse_int(arguments[1], "index"), duration);
        } else if (operation == L"rate") {
            if (count != 2) throw std::invalid_argument("rate 需要 segment-id 和 playback-rate");
            result = ProjectTimelineEditing::setting_playback_rate(
                project, *id, parse_double(arguments[1], "playback-rate"), duration);
        } else {
            throw std::invalid_argument("未知项目编辑操作");
        }
    }
    if (const auto* error = std::get_if<ProjectTimelineEditingError>(&result)) {
        throw std::runtime_error(edit_error_message(*error));
    }
    return std::get<RecorderProject>(std::move(result));
}
}  // namespace

int project_command(const std::filesystem::path& project_path, std::wstring_view action,
                    int argument_count, wchar_t** arguments) {
    if (action == L"list") {
        if (argument_count != 0) throw std::invalid_argument("list 不接受额外参数");
        std::cout << list_projects(project_path).dump(2) << '\n';
        return 0;
    }
    if (action == L"rename") {
        if (argument_count != 1) throw std::invalid_argument("rename 需要 new-name");
        std::cout << rename_project(project_path, arguments[0]).dump(2) << '\n';
        return 0;
    }
    if (action == L"cleanup-output") {
        if (argument_count != 0) throw std::invalid_argument("cleanup-output 不接受额外参数");
        std::error_code error;
        std::filesystem::remove(project_path, error);
        if (error) throw std::system_error(error, "清理导出 partial 失败");
        return 0;
    }
    const auto normalized_path = normalize_project_path(project_path);
    if (action == L"begin") {
        if (argument_count != 0) throw std::invalid_argument("begin 不接受额外参数");
        const bool had_recovery = draft_is_modified(normalized_path);
        if (!had_recovery) {
            std::error_code error;
            std::filesystem::remove(recovery_path(normalized_path), error);
        }
        ensure_draft(normalized_path);
        const auto draft = read_draft(normalized_path);
        const auto duration = full_source_duration(draft);
        auto output = snapshot(draft, duration);
        attach_history(output, normalized_path);
        output["session"]["dirty"] = had_recovery;
        output["session"]["hasRecovery"] = had_recovery;
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    if (action == L"edit-draft") {
        if (argument_count < 1) throw std::invalid_argument("edit-draft 缺少操作");
        ensure_draft(normalized_path);
        const auto draft = read_draft(normalized_path);
        const auto duration = full_source_duration(draft);
        const auto edited = apply_project_operation(draft, arguments[0], argument_count - 1, arguments + 1);
        write_draft(normalized_path, edited);
        auto output = snapshot(edited, duration);
        attach_history(output, normalized_path);
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    if (action == L"save") {
        if (argument_count != 0) throw std::invalid_argument("save 不接受额外参数");
        const auto draft = read_draft(normalized_path);
        const auto duration = full_source_duration(draft);
        commit_project(normalized_path, draft);
        std::error_code error;
        std::filesystem::remove(draft_path(normalized_path), error);
        std::filesystem::remove(recovery_path(normalized_path), error);
        auto output = snapshot(draft, duration);
        attach_history(output, normalized_path);
        output["session"]["dirty"] = false;
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    if (action == L"discard") {
        if (argument_count != 0) throw std::invalid_argument("discard 不接受额外参数");
        std::error_code error;
        std::filesystem::remove(draft_path(normalized_path), error);
        std::filesystem::remove(recovery_path(normalized_path), error);
        const auto project = read_project(normalized_path);
        const auto duration = full_source_duration(project);
        auto output = snapshot(project, duration);
        attach_history(output, normalized_path);
        output["session"]["dirty"] = false;
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    auto project = read_project(normalized_path);
    const auto duration = full_source_duration(project);
    if (action == L"inspect") {
        if (argument_count != 0) throw std::invalid_argument("inspect 不接受额外参数");
        auto output = snapshot(project, duration);
        attach_history(output, normalized_path);
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    if (action == L"undo" || action == L"redo") {
        if (argument_count != 0) throw std::invalid_argument("undo/redo 不接受额外参数");
        change_revision(normalized_path, action == L"redo");
        project = read_project(normalized_path);
        auto output = snapshot(project, duration);
        attach_history(output, normalized_path);
        std::cout << output.dump(2) << '\n';
        return 0;
    }
    if (action != L"edit") throw std::invalid_argument("项目动作仅支持 inspect/edit/undo/redo/rename/begin/edit-draft/save/discard");
    if (argument_count < 1) throw std::invalid_argument("edit 缺少操作");
    const auto edited = apply_project_operation(project, arguments[0], argument_count - 1, arguments + 1);
    commit_project(normalized_path, edited);
    auto output = snapshot(edited, duration);
    attach_history(output, normalized_path);
    std::cout << output.dump(2) << '\n';
    return 0;
}
}  // namespace dogsc::cli
