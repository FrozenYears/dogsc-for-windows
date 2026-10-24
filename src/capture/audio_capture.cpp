#include "dogsc/native_capture.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <winrt/base.h>

#include <bit>
#include <limits>
#include <utility>

namespace dogsc::capture {
namespace {

[[noreturn]] void unsupported_format(const wchar_t* reason) {
    throw winrt::hresult_error(AUDCLNT_E_UNSUPPORTED_FORMAT, reason);
}

AudioFormat describe_format(const WAVEFORMATEX& wave) {
    AudioFormat result;
    result.sample_rate = wave.nSamplesPerSec;
    result.channels = wave.nChannels;
    result.bits_per_sample = wave.wBitsPerSample;
    result.channel_mask = wave.nChannels == 1 ? SPEAKER_FRONT_CENTER
        : wave.nChannels == 2 ? SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT : 0;

    WORD format_tag = wave.wFormatTag;
    if (format_tag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            unsupported_format(L"WASAPI returned an incomplete WAVEFORMATEXTENSIBLE mix format.");
        }
        const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
        if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            format_tag = WAVE_FORMAT_PCM;
        } else if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            format_tag = WAVE_FORMAT_IEEE_FLOAT;
        } else {
            unsupported_format(L"WASAPI mix format is neither PCM nor IEEE floating point.");
        }
        // 共享接口没有有效位字段，不能把容器位宽冒充实际有效位数。
        if (extended.Samples.wValidBitsPerSample != wave.wBitsPerSample) {
            unsupported_format(L"WASAPI mix format has different valid-bit and container widths.");
        }
        result.channel_mask = extended.dwChannelMask;
        if (result.channel_mask != 0 &&
            std::popcount(result.channel_mask) != result.channels) {
            unsupported_format(L"WASAPI mix format has an inconsistent channel mask.");
        }
    }

    if (format_tag == WAVE_FORMAT_IEEE_FLOAT) {
        result.floating_point = true;
        if (wave.wBitsPerSample != 32 && wave.wBitsPerSample != 64) {
            unsupported_format(L"WASAPI IEEE floating-point mix format must use 32 or 64 bits.");
        }
    } else if (format_tag == WAVE_FORMAT_PCM) {
        result.floating_point = false;
        // 无符号 8 位 PCM 的静音不是零，不能按本接口的零填充约定输出。
        if (wave.wBitsPerSample != 16 && wave.wBitsPerSample != 24 &&
            wave.wBitsPerSample != 32) {
            unsupported_format(L"WASAPI PCM mix format must use signed 16, 24 or 32-bit samples.");
        }
    } else {
        unsupported_format(L"WASAPI mix format is neither PCM nor IEEE floating point.");
    }

    const auto block_align = static_cast<std::uint32_t>(wave.nChannels) *
        (wave.wBitsPerSample / 8);
    if (wave.nChannels == 0 || wave.nSamplesPerSec == 0 ||
        block_align != wave.nBlockAlign ||
        static_cast<std::uint64_t>(wave.nSamplesPerSec) * block_align != wave.nAvgBytesPerSec) {
        unsupported_format(L"WASAPI mix format has invalid sample-rate or frame-size fields.");
    }
    return result;
}

class CaptureBuffer {
public:
    explicit CaptureBuffer(IAudioCaptureClient* client) noexcept : client_(client) {}
    CaptureBuffer(const CaptureBuffer&) = delete;
    CaptureBuffer& operator=(const CaptureBuffer&) = delete;

    ~CaptureBuffer() noexcept {
        if (client_) {
            // 异常时归还所有权但不消费包，让调用者决定是否终止录制。
            client_->ReleaseBuffer(0);
        }
    }

    void release(UINT32 frames) {
        auto* client = std::exchange(client_, nullptr);
        winrt::check_hresult(client->ReleaseBuffer(frames));
    }

private:
    IAudioCaptureClient* client_;
};

} // namespace

struct AudioCapture::Impl {
    winrt::com_ptr<IAudioClient> client;
    winrt::com_ptr<IAudioCaptureClient> capture;
    AudioFormat audio_format;
    std::uint16_t block_align = 0;
    bool running = false;

    explicit Impl(AudioSource source) {
        EDataFlow flow;
        DWORD stream_flags = 0;
        switch (source) {
        case AudioSource::system:
            flow = eRender;
            stream_flags = AUDCLNT_STREAMFLAGS_LOOPBACK;
            break;
        case AudioSource::microphone:
            flow = eCapture;
            break;
        default:
            throw winrt::hresult_error(E_INVALIDARG, L"Unknown audio capture source.");
        }

        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        winrt::check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
        winrt::com_ptr<IMMDevice> endpoint;
        winrt::check_hresult(enumerator->GetDefaultAudioEndpoint(flow, eConsole, endpoint.put()));
        winrt::check_hresult(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
            nullptr, client.put_void()));

        WAVEFORMATEX* raw_format = nullptr;
        winrt::check_hresult(client->GetMixFormat(&raw_format));
        const std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mix_format(
            raw_format, &CoTaskMemFree);
        audio_format = describe_format(*mix_format);
        block_align = mix_format->nBlockAlign;

        // 轮询与编码共用调用线程，预留一秒缓冲以容忍短暂的编码阻塞。
        constexpr REFERENCE_TIME buffer_duration = 10'000'000;
        winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
            buffer_duration, 0, mix_format.get(), nullptr));
        winrt::check_hresult(client->GetService(__uuidof(IAudioCaptureClient), capture.put_void()));
    }

    ~Impl() noexcept {
        if (running) {
            client->Stop();
        }
    }
};

AudioCapture::AudioCapture(AudioSource source) : impl_(std::make_unique<Impl>(source)) {}

AudioCapture::~AudioCapture() = default;

const AudioFormat& AudioCapture::format() const {
    return impl_->audio_format;
}

void AudioCapture::start() {
    if (!impl_->running) {
        winrt::check_hresult(impl_->client->Start());
        impl_->running = true;
    }
}

std::vector<AudioPacket> AudioCapture::drain() {
    std::vector<AudioPacket> packets;
    for (;;) {
        UINT32 next_frames = 0;
        winrt::check_hresult(impl_->capture->GetNextPacketSize(&next_frames));
        if (next_frames == 0) {
            break;
        }

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 qpc_position = 0;
        const HRESULT status = impl_->capture->GetBuffer(
            &data, &frames, &flags, nullptr, &qpc_position);
        winrt::check_hresult(status);
        if (status == AUDCLNT_S_BUFFER_EMPTY || frames == 0) {
            break;
        }
        CaptureBuffer buffer(impl_->capture.get());

        // 错误标志下连设备位置也不保证有效，明确失败而不是猜测同步时间。
        if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0) {
            throw winrt::hresult_error(E_FAIL,
                L"WASAPI reported an unreliable audio timestamp; refusing to guess A/V synchronization.");
        }
        if (qpc_position > static_cast<UINT64>(std::numeric_limits<std::int64_t>::max())) {
            throw winrt::hresult_error(E_FAIL, L"WASAPI audio timestamp is outside the supported range.");
        }

        AudioPacket packet;
        packet.frames = frames;
        // GetBuffer 已换算成 QPC 原点的 100ns 单位，不能再次按 QPC 频率换算。
        packet.host_time = static_cast<std::int64_t>(qpc_position);
        packet.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        const auto byte_count = static_cast<std::size_t>(frames) * impl_->block_align;
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
            packet.bytes.resize(byte_count, 0);
        } else {
            if (!data) {
                throw winrt::hresult_error(E_POINTER, L"WASAPI returned a null non-silent audio buffer.");
            }
            // 从端点缓冲直接构造最终载荷，仅分配一次，不先清零再复制。
            packet.bytes.assign(data, data + byte_count);
        }
        buffer.release(frames);
        packets.push_back(std::move(packet));
    }
    return packets;
}

void AudioCapture::stop() {
    if (impl_->running) {
        winrt::check_hresult(impl_->client->Stop());
        impl_->running = false;
    }
}

} // namespace dogsc::capture
