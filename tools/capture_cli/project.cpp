#include "commands.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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

void write_project(const std::filesystem::path& path, const RecorderProject& project) {
    const auto temporary = path.parent_path() / (path.filename().wstring() + L".dogsc-edit.tmp");
    const auto previous = path.parent_path() / L"project.previous.json";
    const std::string text = ProjectJson::encode(project);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("无法创建工程临时文件");
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            std::filesystem::remove(temporary, ignored);
            throw std::runtime_error("工程临时文件写入失败");
        }
    }
    if (!std::filesystem::copy_file(path, previous,
            std::filesystem::copy_options::overwrite_existing, ignored)) {
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("无法保留 project.previous.json");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("工程原子替换失败");
    }
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
    const auto normalized_path = normalize_project_path(project_path);
    const auto project = read_project(normalized_path);
    const auto duration = full_source_duration(project);
    if (action == L"inspect") {
        if (argument_count != 0) throw std::invalid_argument("inspect 不接受额外参数");
        std::cout << snapshot(project, duration).dump(2) << '\n';
        return 0;
    }
    if (action != L"edit") throw std::invalid_argument("项目动作仅支持 inspect/edit");
    if (argument_count < 1) throw std::invalid_argument("edit 缺少操作");
    const auto edited = apply_edit(project, arguments[0], argument_count - 1, arguments + 1);
    write_project(normalized_path, edited);
    std::cout << snapshot(edited, duration).dump(2) << '\n';
    return 0;
}
}  // namespace dogsc::cli
