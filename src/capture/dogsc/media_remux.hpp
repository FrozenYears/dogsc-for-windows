#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace dogsc::capture {
struct MediaSegment {
    std::filesystem::path path;
    std::int64_t output_start = 0;
    std::int64_t duration = 0;
};

// 对独立 MP4 分段的压缩样本重定时后无损封装，不创建解码器或编码器。
// 每段都是同一录制格式，输入已完整 finalize；输出拒绝覆盖既有文件。
void remux_segments(const std::vector<MediaSegment>& segments,
                    const std::filesystem::path& output);
} // namespace dogsc::capture
