#include "commands.hpp"

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

void attach_history(json& result, const std::filesystem::path& path) {
    const auto history = read_history(path);
    result["history"] = {
        {"canUndo", !history.states.empty() && history.current > 0},
        {"canRedo", !history.states.empty() && history.current + 1 < history.states.size()},
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
RecorderProject apply_edit(const RecorderProject& project, std::wstring_view operation,
                           int count, wchar_t** arguments) {
    const auto duration = full_source_duration(project);
    if (count < 1) throw std::invalid_argument("项目 edit 缺少操作参数");
    auto result = ProjectTimelineEditResult{};
    if (operation == L"split") {
        if (count != 1) throw std::invalid_argument("split 需要 output-time");
        result = ProjectTimelineEditing::split_primary_segment(
            project, parse_double(arguments[0], "output-time"), new_uuid(project), duration);
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
    const auto normalized_path = normalize_project_path(project_path);
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
    if (action != L"edit") throw std::invalid_argument("项目动作仅支持 inspect/edit/undo/redo/rename");
    if (argument_count < 1) throw std::invalid_argument("edit 缺少操作");
    const auto edited = apply_edit(project, arguments[0], argument_count - 1, arguments + 1);
    commit_project(normalized_path, edited);
    auto output = snapshot(edited, duration);
    attach_history(output, normalized_path);
    std::cout << output.dump(2) << '\n';
    return 0;
}
}  // namespace dogsc::cli
