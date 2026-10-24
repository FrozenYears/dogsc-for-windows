#pragma once

#include "dogsc/media_remux.hpp"
#include "dogsc/native_capture.hpp"

#include <memory>
#include <string>

namespace dogsc::capture {

class FFmpegProResWriter {
public:
    FFmpegProResWriter(ID3D11Device* device, const WriterOptions& options);
    ~FFmpegProResWriter();
    FFmpegProResWriter(const FFmpegProResWriter&) = delete;
    FFmpegProResWriter& operator=(const FFmpegProResWriter&) = delete;
    void write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration);
    void write_audio(std::size_t track, const std::uint8_t* data, std::uint32_t frames,
                     std::int64_t time);
    void finalize();
    std::string encoder_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string ffmpeg_prores_encoder_name();
void ffmpeg_remux_segments(const std::vector<MediaSegment>& segments,
                           const std::filesystem::path& output);

} // namespace dogsc::capture
