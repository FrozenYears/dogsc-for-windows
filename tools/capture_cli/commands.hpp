#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include "dogsc/screen_capture.hpp"

namespace dogsc::cli {
struct RecordOptions {
    std::filesystem::path directory;
    capture::CaptureTarget target;
    int frame_rate = 60;
    double duration = 0;
    double pause_at = -1;
    double pause_for = 0;
    std::string codec = "auto";
    bool system_audio = false;
    bool microphone = false;
    bool camera = false;
};
int record(const RecordOptions& options);
int probe(const std::filesystem::path& directory);
void list_targets();
void list_encoders();
int project_command(const std::filesystem::path& project_path, std::wstring_view action,
                    int argument_count, wchar_t** arguments);
} // namespace dogsc::cli
