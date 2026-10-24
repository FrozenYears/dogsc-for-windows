#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <mmreg.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dogsc::capture {

// 原始采集时间戳使用 QPC 原点、100ns 单位，与 WGC/WASAPI 一致。
std::int64_t host_time_100ns();

struct AudioFormat {
    std::uint32_t sample_rate = 48000;
    std::uint16_t channels = 2;
    std::uint16_t bits_per_sample = 32;
    bool floating_point = true;
    std::uint32_t channel_mask = 3;
};

struct AudioPacket {
    std::vector<std::uint8_t> bytes;
    std::int64_t host_time = 0;
    std::uint32_t frames = 0;
    bool discontinuity = false;
};

enum class AudioSource { system, microphone };

class AudioCapture {
public:
    explicit AudioCapture(AudioSource source);
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    const AudioFormat& format() const;
    void start();
    std::vector<AudioPacket> drain();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class VideoCodec { hevc, h264 };

struct EncoderInfo {
    std::string name;
    VideoCodec codec = VideoCodec::h264;
    bool hardware = false;
    bool activation_succeeded = false;
    std::int32_t activation_hresult = 0;
};

// 调用者已初始化 COM 和 MF；枚举与激活不等于已证明编码成功。
std::vector<EncoderInfo> enumerate_video_encoders();

struct WriterOptions {
    std::wstring path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_rate = 60;
    VideoCodec codec = VideoCodec::hevc;
    bool fragmented = true;
    std::vector<AudioFormat> audio_formats;
};

// 单线程调用；write_video 不持有调用者的纹理，返回后即可复用。
// 写入时间由调用者通过共享 RecordingClock 归零并扣除暂停，不得单轨另选原点。
// width/height 均为零时创建纯音频 writer，device 可以为空。
class MediaWriter {
public:
    MediaWriter(ID3D11Device* device, const WriterOptions& options);
    ~MediaWriter();
    MediaWriter(const MediaWriter&) = delete;
    MediaWriter& operator=(const MediaWriter&) = delete;
    void write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration);
    void write_audio(std::size_t track, const std::uint8_t* data, std::uint32_t frames,
                     std::int64_t time);
    void finalize();
    std::string video_encoder_name() const;
    bool video_encoder_is_hardware() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dogsc::capture
