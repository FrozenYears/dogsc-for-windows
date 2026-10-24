#pragma once

#include "dogsc/native_capture.hpp"
#include <filesystem>

namespace dogsc::capture {

// 独立完成的普通 MP4 分段；结束时按压缩样本无损拼接。
// writer 时间戳与 MediaWriter 一样，使用已归零、已扣除暂停的共享时钟。
class SegmentedWriter {
public:
    SegmentedWriter(ID3D11Device* device, WriterOptions options);
    ~SegmentedWriter();
    SegmentedWriter(const SegmentedWriter&) = delete;
    SegmentedWriter& operator=(const SegmentedWriter&) = delete;
    void write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration);
    void write_audio(std::size_t track, const std::uint8_t* data, std::uint32_t frames,
                     std::int64_t time);
    void finalize(std::int64_t duration);
    std::string video_encoder_name() const;
    bool video_encoder_is_hardware() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 只读取日志中已完整 finalize 的连续前缀；活动分段不冒充可恢复媒体。
void recover_segments(const std::filesystem::path& parts_directory,
                      const std::filesystem::path& new_output);
} // namespace dogsc::capture
