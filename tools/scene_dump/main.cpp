#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dogsc/frame_scene_evaluator.hpp"
#include "dogsc/output_frame_schedule.hpp"
#include "dogsc/project_json.hpp"

namespace {

std::string read_file(const std::string& path, bool& ok) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ok = false;
        return {};
    }
    std::string text;
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, read);
    }
    std::fclose(file);
    ok = true;
    return text;
}

const char* layer_name(dogsc::FrameLayerRole role) {
    switch (role) {
        case dogsc::FrameLayerRole::kBackground:
            return "background";
        case dogsc::FrameLayerRole::kScreen:
            return "screen";
        case dogsc::FrameLayerRole::kCursor:
            return "cursor";
        case dogsc::FrameLayerRole::kSpotlight:
            return "spotlight";
        case dogsc::FrameLayerRole::kCamera:
            return "camera";
        case dogsc::FrameLayerRole::kStickers:
            return "stickers";
    }
    return "unknown";
}

/// `scene-dump`：把工程转成逐帧场景 JSON。
///
/// 它是 M0 的回归对照物：同一份工程必须永远产出相同的字段值。
/// 只输出与渲染决策相关的字段，不引入任何依赖机器状态的计算。
int run(const std::string& project_path, double output_duration, int frame_rate) {
    bool ok = false;
    const std::string text = read_file(project_path, ok);
    if (!ok) {
        std::fprintf(stderr, "无法打开工程文件: %s\n", project_path.c_str());
        return 2;
    }

    dogsc::RecorderProject project;
    dogsc::ProjectJsonError error;
    if (!dogsc::ProjectJson::decode(text, project, error)) {
        std::fprintf(stderr, "工程解析失败: %s %s\n", error.path.c_str(), error.detail.c_str());
        return 3;
    }

    dogsc::OutputFrameRate rate = dogsc::OutputFrameRate::fps60;
    switch (frame_rate) {
        case 30:
            rate = dogsc::OutputFrameRate::fps30;
            break;
        case 60:
            rate = dogsc::OutputFrameRate::fps60;
            break;
        case 90:
            rate = dogsc::OutputFrameRate::fps90;
            break;
        case 120:
            rate = dogsc::OutputFrameRate::fps120;
            break;
        default:
            std::fprintf(stderr, "不支持的帧率: %d\n", frame_rate);
            return 4;
    }

    dogsc::OutputFrameSchedule schedule;
    if (dogsc::OutputFrameSchedule::create(output_duration, rate, schedule).has_value()) {
        std::fprintf(stderr, "输出时长非法: %f\n", output_duration);
        return 5;
    }

    const bool has_cursor = !(project.cursor_style.asset_id == dogsc::CursorAssetID::hidden());
    const bool has_camera = !project.camera.is_hidden;

    nlohmann::ordered_json frames = nlohmann::ordered_json::array();
    for (int index = 0; index < schedule.frame_count(); ++index) {
        const auto frame = schedule.frame_at(index);
        if (!frame.has_value()) continue;
        const dogsc::FrameRenderPlan plan = dogsc::FrameSceneEvaluator::render_plan(
            frame->presentation_time_seconds(), output_duration, frame_rate,
            dogsc::CompositionSize{1920, 1080}, has_cursor, false, has_camera, false);
        std::vector<std::string> layers;
        layers.reserve(plan.scene.layer_order.size());
        for (dogsc::FrameLayerRole role : plan.scene.layer_order) {
            layers.emplace_back(layer_name(role));
        }
        frames.push_back(nlohmann::ordered_json{
            {"index", index},
            {"ptsValue", frame->presentation_time_value},
            {"ptsTimescale", frame->presentation_timescale},
            {"time", plan.presentation_time},
            {"layers", layers},
        });
    }

    nlohmann::ordered_json root;
    root["frameRate"] = frame_rate;
    root["outputDuration"] = output_duration;
    root["frameCount"] = schedule.frame_count();
    root["ticksPerFrame"] = schedule.ticks_per_frame();
    root["frames"] = frames;
    std::printf("%s\n", root.dump(2).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "用法: scene_dump <project.json> <output-duration-seconds> <frame-rate>\n");
        return 1;
    }
    return run(argv[1], std::atof(argv[2]), std::atoi(argv[3]));
}
