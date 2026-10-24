#include "dogsc/ffmpeg_prores.hpp"
#include <d3d11.h>
#include <winrt/base.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace dogsc::capture {
namespace {
constexpr std::int64_t ticks_per_second = 10'000'000;
constexpr AVRational capture_time_base{1, static_cast<int>(ticks_per_second)};
constexpr int output_rate = 48'000;
constexpr int output_bitrate = 192'000;

std::string ffmpeg_error(int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(code, buffer.data(), buffer.size());
    return buffer.data();
}

void require_ffmpeg(int result, const char* operation) {
    if (result < 0) {
        throw std::runtime_error(std::string(operation) + ": " + ffmpeg_error(result));
    }
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

AVSampleFormat input_sample_format(const AudioFormat& format) {
    if (format.floating_point) {
        return format.bits_per_sample == 32 ? AV_SAMPLE_FMT_FLT : AV_SAMPLE_FMT_DBL;
    }
    if (format.bits_per_sample == 16) return AV_SAMPLE_FMT_S16;
    return AV_SAMPLE_FMT_S32;
}

AVChannelLayout audio_channel_layout(const AudioFormat& format) {
    AVChannelLayout layout{};
    int result = 0;
    if (format.channel_mask) {
        result = av_channel_layout_from_mask(&layout, format.channel_mask);
    } else {
        av_channel_layout_default(&layout, format.channels);
    }
    if (result < 0 || layout.nb_channels != format.channels) {
        av_channel_layout_uninit(&layout);
        throw std::invalid_argument("WASAPI audio channel layout is invalid for FFmpeg AAC conversion");
    }
    return layout;
}

void set_video_color(AVCodecContext* context) {
    context->color_range = AVCOL_RANGE_MPEG;
    context->color_primaries = AVCOL_PRI_BT709;
    context->color_trc = AVCOL_TRC_BT709;
    context->colorspace = AVCOL_SPC_BT709;
    context->chroma_sample_location = AVCHROMA_LOC_LEFT;
}

void set_video_color(AVCodecParameters* parameters) {
    parameters->color_range = AVCOL_RANGE_MPEG;
    parameters->color_primaries = AVCOL_PRI_BT709;
    parameters->color_trc = AVCOL_TRC_BT709;
    parameters->color_space = AVCOL_SPC_BT709;
    parameters->chroma_location = AVCHROMA_LOC_LEFT;
}

bool codec_supports_profile(const AVCodec* codec, int profile) {
    if (!codec->profiles) return false;
    for (const auto* entry = codec->profiles; entry->profile != AV_PROFILE_UNKNOWN; ++entry) {
        if (entry->profile == profile) return true;
    }
    return false;
}

bool codec_supports_pixel_format(const AVCodec* codec, AVPixelFormat format) {
    const void* supported = nullptr;
    int count = 0;
    require_ffmpeg(avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT,
        0, &supported, &count), "Querying ProRes pixel formats failed");
    const auto* formats = static_cast<const AVPixelFormat*>(supported);
    return formats && std::find(formats, formats + count, format) != formats + count;
}


AVCodecContext* open_encoder(const AVCodec* codec, AVFormatContext* output) {
    auto* context = avcodec_alloc_context3(codec);
    if (!context) throw std::bad_alloc();
    if (output->oformat->flags & AVFMT_GLOBALHEADER) context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    return context;
}

void copy_packet_metadata(AVStream* destination, const AVStream* source) {
    require_ffmpeg(av_dict_copy(&destination->metadata, source->metadata, 0),
        "Copying MOV stream metadata failed");
    destination->disposition = source->disposition;
    destination->time_base = source->time_base;
    destination->avg_frame_rate = source->avg_frame_rate;
    destination->r_frame_rate = source->r_frame_rate;
}

bool compatible_codec_parameters(const AVCodecParameters* left, const AVCodecParameters* right) {
    if (left->codec_type != right->codec_type || left->codec_id != right->codec_id
        || left->codec_tag != right->codec_tag || left->format != right->format
        || left->profile != right->profile || left->width != right->width
        || left->height != right->height || left->sample_rate != right->sample_rate
        || av_channel_layout_compare(&left->ch_layout, &right->ch_layout) != 0
        || left->extradata_size != right->extradata_size) {
        return false;
    }
    return left->extradata_size == 0
        || std::memcmp(left->extradata, right->extradata, left->extradata_size) == 0;
}

void close_format(AVFormatContext*& format) noexcept {
    if (!format) return;
    if (format->pb) avio_closep(&format->pb);
    avformat_free_context(format);
    format = nullptr;
}
}

std::string ffmpeg_prores_encoder_name() {
    const auto* encoder = avcodec_find_encoder_by_name("prores_ks");
    if (!encoder || !codec_supports_profile(encoder, AV_PROFILE_PRORES_STANDARD)
        || !codec_supports_pixel_format(encoder, AV_PIX_FMT_YUV422P10)) {
        throw std::runtime_error("FFmpeg prores_ks lacks the ProRes 422 Standard profile or native YUV422P10 format");
    }
    return encoder->name;
}

struct FFmpegProResWriter::Impl {
    struct AudioTrack {
        AudioFormat input;
        AVChannelLayout input_layout{};
        AVCodecContext* encoder = nullptr;
        AVStream* stream = nullptr;
        SwrContext* resampler = nullptr;
        AVFrame* resample_frame = nullptr;
        AVFrame* encode_frame = nullptr;
        int resample_capacity = 0;
        std::int64_t next_pts = AV_NOPTS_VALUE;
        std::int64_t encoded_pts = AV_NOPTS_VALUE;
        AVAudioFifo* fifo = nullptr;
        std::vector<std::int32_t> pcm24;
        AudioTrack() = default;
        AudioTrack(const AudioTrack&) = delete;
        AudioTrack& operator=(const AudioTrack&) = delete;
        AudioTrack(AudioTrack&& other) noexcept
            : input(other.input), input_layout(other.input_layout), encoder(std::exchange(other.encoder, nullptr)),
              stream(other.stream), resampler(std::exchange(other.resampler, nullptr)),
              resample_frame(std::exchange(other.resample_frame, nullptr)),
              encode_frame(std::exchange(other.encode_frame, nullptr)),
              resample_capacity(other.resample_capacity), next_pts(other.next_pts), encoded_pts(other.encoded_pts),
              fifo(std::exchange(other.fifo, nullptr)), pcm24(std::move(other.pcm24)) {
            other.input_layout = {};
        }

        ~AudioTrack() {
            if (encoder) avcodec_free_context(&encoder);
            if (resampler) swr_free(&resampler);
            if (resample_frame) av_frame_free(&resample_frame);
            if (encode_frame) av_frame_free(&encode_frame);
            if (fifo) av_audio_fifo_free(fifo);
            av_channel_layout_uninit(&input_layout);
        }
    };
    AVPacket* encode_packet = nullptr;

    WriterOptions options;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    AVFormatContext* format = nullptr;
    AVCodecContext* video_encoder = nullptr;
    AVStream* video_stream = nullptr;
    AVFrame* video_frame = nullptr;
    SwsContext* scaler = nullptr;
    std::vector<AudioTrack> audio;
    std::int64_t next_video_pts = 0;
    std::int64_t next_video_packet_pts = 0;
    std::uint64_t video_packet_count = 0;
    bool finalized = false;
    bool header_written = false;
    std::string encoder;

    explicit Impl(ID3D11Device* input_device, const WriterOptions& requested) : options(requested) {
        initialize(input_device);
    }

    ~Impl() {
        if (!finalized && header_written) {
            try { finalize(); } catch (...) { }
        }
        if (encode_packet) av_packet_free(&encode_packet);
        if (scaler) sws_freeContext(scaler);
        if (video_frame) av_frame_free(&video_frame);
        if (video_encoder) avcodec_free_context(&video_encoder);
        audio.clear();
        close_format(format);
    }

    void initialize(ID3D11Device* input_device) {
        if (options.path.empty() || !options.width || !options.height || !options.frame_rate || !input_device) {
            throw std::invalid_argument("ProRes recording requires an output path, D3D11 device, dimensions and frame rate");
        }
        if (std::filesystem::exists(options.path)) throw std::runtime_error("Media output file already exists");
        const auto path = path_utf8(options.path);
        require_ffmpeg(avformat_alloc_output_context2(&format, nullptr, "mov", path.c_str()),
            "Allocating ProRes MOV muxer failed");
        if (!format) throw std::runtime_error("FFmpeg did not create a MOV output context");
        encode_packet = av_packet_alloc();
        if (!encode_packet) throw std::bad_alloc();
        encoder = ffmpeg_prores_encoder_name();
        const auto* codec = avcodec_find_encoder_by_name(encoder.c_str());
        video_encoder = open_encoder(codec, format);
        video_encoder->width = static_cast<int>(options.width);
        video_encoder->height = static_cast<int>(options.height);
        video_encoder->time_base = AVRational{1, static_cast<int>(options.frame_rate)};
        video_encoder->framerate = AVRational{static_cast<int>(options.frame_rate), 1};
        video_encoder->pix_fmt = AV_PIX_FMT_YUV422P10;
        video_encoder->profile = AV_PROFILE_PRORES_STANDARD;
        video_encoder->thread_count = 0;
        video_encoder->thread_type = FF_THREAD_SLICE;
        set_video_color(video_encoder);
        AVDictionary* encoder_options = nullptr;
        const int profile_option_result = av_dict_set(&encoder_options, "profile", "standard", 0);
        if (profile_option_result < 0) {
            av_dict_free(&encoder_options);
            require_ffmpeg(profile_option_result, "Setting ProRes 422 Standard profile failed");
        }
        const int open_result = avcodec_open2(video_encoder, codec, &encoder_options);
        const bool unconsumed_profile_option = encoder_options != nullptr;
        av_dict_free(&encoder_options);
        require_ffmpeg(open_result, "Opening FFmpeg ProRes 422 Standard encoder failed");
        if (unconsumed_profile_option) {
            throw std::runtime_error("FFmpeg ProRes encoder did not consume the standard profile option");
        }
        if (video_encoder->profile != AV_PROFILE_PRORES_STANDARD
            || video_encoder->active_thread_type != FF_THREAD_SLICE
            || video_encoder->thread_count <= 1) {
            throw std::runtime_error("FFmpeg ProRes encoder did not activate the standard profile with slice threading");
        }
        video_stream = avformat_new_stream(format, nullptr);
        if (!video_stream) throw std::bad_alloc();
        video_stream->time_base = video_encoder->time_base;
        video_stream->avg_frame_rate = video_encoder->framerate;
        require_ffmpeg(avcodec_parameters_from_context(video_stream->codecpar, video_encoder),
            "Copying ProRes codec parameters failed");
        video_stream->codecpar->codec_tag = MKTAG('a', 'p', 'c', 'n');
        set_video_color(video_stream->codecpar);

        audio.reserve(options.audio_formats.size());
        for (const auto& input : options.audio_formats) initialize_audio(input);
        scaler = sws_getContext(static_cast<int>(options.width), static_cast<int>(options.height), AV_PIX_FMT_BGRA,
            static_cast<int>(options.width), static_cast<int>(options.height), AV_PIX_FMT_YUV422P10,
            SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!scaler) throw std::runtime_error("Creating BGRA to YUV422P10LE color converter failed");
        const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
        require_ffmpeg(sws_setColorspaceDetails(scaler, coefficients, 1, coefficients, 0,
            0, 1 << 16, 1 << 16), "Configuring BT.709 limited-range screen conversion failed");

        video_frame = av_frame_alloc();
        if (!video_frame) throw std::bad_alloc();
        video_frame->format = video_encoder->pix_fmt;
        video_frame->width = video_encoder->width;
        video_frame->height = video_encoder->height;
        video_frame->color_range = video_encoder->color_range;
        video_frame->color_primaries = video_encoder->color_primaries;
        video_frame->color_trc = video_encoder->color_trc;
        video_frame->colorspace = video_encoder->colorspace;
        video_frame->chroma_location = video_encoder->chroma_sample_location;
        require_ffmpeg(av_frame_get_buffer(video_frame, 32), "Allocating ProRes video frame failed");

        device.copy_from(input_device);
        device->GetImmediateContext(context.put());
        D3D11_TEXTURE2D_DESC description{};
        description.Width = options.width;
        description.Height = options.height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));

        if (!(format->oformat->flags & AVFMT_NOFILE)) {
            require_ffmpeg(avio_open(&format->pb, path.c_str(), AVIO_FLAG_WRITE), "Opening ProRes MOV output failed");
        }
        require_ffmpeg(avformat_write_header(format, nullptr), "Writing ProRes MOV header failed");
        if (video_stream->codecpar->codec_tag != MKTAG('a', 'p', 'c', 'n')) {
            throw std::runtime_error("MOV muxer did not retain the ProRes 422 Standard apcn tag");
        }
        header_written = true;
    }

    void initialize_audio(const AudioFormat& input) {
        if (!input.sample_rate || !input.channels || !input.bits_per_sample
            || (input.floating_point && input.bits_per_sample != 32 && input.bits_per_sample != 64)
            || (!input.floating_point && input.bits_per_sample != 16
                && input.bits_per_sample != 24 && input.bits_per_sample != 32)) {
            throw std::invalid_argument("WASAPI PCM format cannot be encoded as 48 kHz stereo AAC");
        }
        audio.emplace_back();
        auto& track = audio.back();
        track.input = input;
        track.input_layout = audio_channel_layout(input);
        const auto* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec) throw std::runtime_error("FFmpeg built-in AAC encoder is unavailable");
        track.encoder = open_encoder(codec, format);
        track.encoder->sample_rate = output_rate;
        track.encoder->bit_rate = output_bitrate;
        track.encoder->time_base = AVRational{1, output_rate};
        const void* supported_formats = nullptr;
        int supported_format_count = 0;
        require_ffmpeg(avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
            0, &supported_formats, &supported_format_count), "Querying AAC sample formats failed");
        const auto* formats = static_cast<const AVSampleFormat*>(supported_formats);
        track.encoder->sample_fmt = formats && supported_format_count > 0
            ? formats[0] : AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&track.encoder->ch_layout, 2);
        require_ffmpeg(avcodec_open2(track.encoder, codec, nullptr), "Opening 48 kHz stereo AAC encoder failed");
        track.stream = avformat_new_stream(format, nullptr);
        if (!track.stream) throw std::bad_alloc();
        track.stream->time_base = track.encoder->time_base;
        require_ffmpeg(avcodec_parameters_from_context(track.stream->codecpar, track.encoder),
            "Copying AAC codec parameters failed");
        require_ffmpeg(swr_alloc_set_opts2(&track.resampler, &track.encoder->ch_layout,
            track.encoder->sample_fmt, track.encoder->sample_rate, &track.input_layout,
            input_sample_format(input), static_cast<int>(input.sample_rate), 0, nullptr),
            "Creating WASAPI to AAC resampler failed");
        require_ffmpeg(swr_init(track.resampler), "Initializing WASAPI to AAC resampler failed");
        const int frame_size = track.encoder->frame_size;
        if (frame_size <= 0 || frame_size > std::numeric_limits<int>::max() / 4) {
            throw std::runtime_error("FFmpeg AAC encoder reported an invalid frame size");
        }
        track.fifo = av_audio_fifo_alloc(track.encoder->sample_fmt, 2, frame_size * 4);
        if (!track.fifo) throw std::bad_alloc();
        track.resample_frame = av_frame_alloc();
        track.encode_frame = av_frame_alloc();
        if (!track.resample_frame || !track.encode_frame) throw std::bad_alloc();
        track.encode_frame->format = track.encoder->sample_fmt;
        track.encode_frame->sample_rate = track.encoder->sample_rate;
        track.encode_frame->nb_samples = frame_size;
        require_ffmpeg(av_channel_layout_copy(&track.encode_frame->ch_layout, &track.encoder->ch_layout),
            "Copying AAC encoder frame channel layout failed");
        require_ffmpeg(av_frame_get_buffer(track.encode_frame, 0), "Allocating AAC encoder frame failed");
    }

    void prepare_resample_frame(AudioTrack& track, int sample_capacity) {
        auto* frame = track.resample_frame;
        if (sample_capacity > track.resample_capacity) {
            av_frame_unref(frame);
            frame->format = track.encoder->sample_fmt;
            frame->sample_rate = track.encoder->sample_rate;
            frame->nb_samples = sample_capacity;
            require_ffmpeg(av_channel_layout_copy(&frame->ch_layout, &track.encoder->ch_layout),
                "Copying resampler frame channel layout failed");
            require_ffmpeg(av_frame_get_buffer(frame, 0), "Allocating resampler output frame failed");
            track.resample_capacity = sample_capacity;
        } else {
            frame->format = track.encoder->sample_fmt;
            frame->sample_rate = track.encoder->sample_rate;
            frame->nb_samples = sample_capacity;
            require_ffmpeg(av_frame_make_writable(frame), "Making resampler output frame writable failed");
        }
    }

    void mux_packet(AVCodecContext* codec, AVStream* stream, AVPacket* packet,
                    std::int64_t& last_dts, bool video) {
        if (video) {
            if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE
                || packet->pts != packet->dts || packet->pts != next_video_packet_pts) {
                throw std::runtime_error("ProRes encoder emitted missing, reordered or non-cadence packet timestamps");
            }
            ++next_video_packet_pts;
            if (packet->duration <= 0) packet->duration = 1;
            ++video_packet_count;
        } else {
            if (packet->dts != AV_NOPTS_VALUE && last_dts != AV_NOPTS_VALUE && packet->dts < last_dts) {
                throw std::runtime_error("AAC encoder emitted non-monotonic packet timestamps");
            }
            if (packet->dts != AV_NOPTS_VALUE) last_dts = packet->dts;
        }
        av_packet_rescale_ts(packet, codec->time_base, stream->time_base);
        packet->stream_index = stream->index;
        packet->pos = -1;
        require_ffmpeg(av_interleaved_write_frame(format, packet), "Muxing ProRes MOV packet failed");
        av_packet_unref(packet);
    }

    void drain_encoder(AVCodecContext* codec, AVStream* stream,
                       std::int64_t& last_dts, bool video, bool flushing) {
        while (true) {
            const int result = avcodec_receive_packet(codec, encode_packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                if (flushing && result == AVERROR(EAGAIN)) {
                    throw std::runtime_error("FFmpeg encoder stopped before completing its flush");
                }
                break;
            }
            require_ffmpeg(result, video ? "Receiving ProRes packet failed" : "Receiving AAC packet failed");
            mux_packet(codec, stream, encode_packet, last_dts, video);
        }
    }
    void encode_audio_fifo(std::size_t index, bool flushing) {
        auto& track = audio[index];
        const int frame_size = track.encoder->frame_size;
        while (true) {
            const int available = av_audio_fifo_size(track.fifo);
            if (available < 0) throw std::runtime_error("Reading AAC sample FIFO size failed");
            if (available == 0 || (!flushing && available < frame_size)) break;
            const int samples_to_read = std::min(available, frame_size);
            const bool small_last_frame = (track.encoder->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;
            const int encoded_samples = samples_to_read < frame_size && !small_last_frame
                ? frame_size : samples_to_read;
            auto* frame = track.encode_frame;
            require_ffmpeg(av_frame_make_writable(frame), "Making AAC encoder frame writable failed");
            frame->nb_samples = encoded_samples;
            const int read = av_audio_fifo_read(track.fifo, reinterpret_cast<void**>(frame->data), samples_to_read);
            if (read != samples_to_read) throw std::runtime_error("Reading complete AAC samples from FIFO failed");
            if (encoded_samples > samples_to_read) {
                require_ffmpeg(av_samples_set_silence(frame->data, samples_to_read,
                    encoded_samples - samples_to_read, 2, track.encoder->sample_fmt),
                    "Padding final AAC frame failed");
            }
            if (track.encoded_pts == AV_NOPTS_VALUE) track.encoded_pts = track.next_pts;
            frame->pts = track.encoded_pts;
            track.encoded_pts += encoded_samples;
            require_ffmpeg(avcodec_send_frame(track.encoder, frame), "Sending AAC frame failed");
            if (audio_last_dts.size() < audio.size()) audio_last_dts.resize(audio.size(), AV_NOPTS_VALUE);
            drain_encoder(track.encoder, track.stream, audio_last_dts[index], false, false);
        }
    }

    void queue_audio_frame(std::size_t index, AVFrame* frame, bool flushing) {
        auto& track = audio[index];
        const int queued = av_audio_fifo_size(track.fifo);
        if (queued < 0 || frame->nb_samples > std::numeric_limits<int>::max() - queued) {
            throw std::runtime_error("AAC sample FIFO capacity overflow");
        }
        const int required = queued + frame->nb_samples;
        require_ffmpeg(av_audio_fifo_realloc(track.fifo, required), "Growing AAC sample FIFO failed");
        const int written = av_audio_fifo_write(track.fifo, reinterpret_cast<void**>(frame->data), frame->nb_samples);
        if (written != frame->nb_samples) throw std::runtime_error("Writing resampled samples into AAC FIFO failed");
        encode_audio_fifo(index, flushing);
    }


    std::int64_t video_last_dts = AV_NOPTS_VALUE;
    std::vector<std::int64_t> audio_last_dts;

    void send_video_frame(std::int64_t time) {
        const auto pts = av_rescale_q(time, capture_time_base, video_encoder->time_base);
        if (time < 0 || pts != next_video_pts) {
            throw std::invalid_argument("ProRes video timestamps must follow the configured constant frame cadence");
        }
        require_ffmpeg(av_frame_make_writable(video_frame), "Making ProRes frame writable failed");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result = context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
        winrt::check_hresult(map_result);
        const std::uint8_t* source[] = {static_cast<const std::uint8_t*>(mapped.pData), nullptr, nullptr, nullptr};
        const int source_stride[] = {static_cast<int>(mapped.RowPitch), 0, 0, 0};
        const int rows = sws_scale(scaler, source, source_stride, 0, video_encoder->height,
            video_frame->data, video_frame->linesize);
        context->Unmap(staging.get(), 0);
        if (rows != video_encoder->height) throw std::runtime_error("FFmpeg swscale converted an incomplete video frame");
        video_frame->pts = pts;
        require_ffmpeg(avcodec_send_frame(video_encoder, video_frame), "Sending ProRes video frame failed");
        drain_encoder(video_encoder, video_stream, video_last_dts, true, false);
        ++next_video_pts;
    }

    void write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration) {
        if (finalized || !bgra || duration <= 0) throw std::invalid_argument("ProRes video input is invalid or finalized");
        D3D11_TEXTURE2D_DESC source{};
        bgra->GetDesc(&source);
        winrt::com_ptr<ID3D11Device> source_device;
        bgra->GetDevice(source_device.put());
        if (source_device != device || source.Format != DXGI_FORMAT_B8G8R8A8_UNORM
            || source.Width < options.width || source.Height < options.height
            || source.SampleDesc.Count != 1 || source.ArraySize != 1) {
            throw std::invalid_argument("ProRes input must be a same-device, single-sample BGRA texture of sufficient size");
        }
        const D3D11_BOX region{0, 0, 0, options.width, options.height, 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, bgra, 0, &region);
        context->Flush();
        send_video_frame(time);
    }

    void write_audio(std::size_t index, const std::uint8_t* data, std::uint32_t frames, std::int64_t time) {
        if (finalized || index >= audio.size() || time < 0 || (frames && !data)) {
            throw std::invalid_argument("ProRes AAC input track, data or timestamp is invalid");
        }
        if (!frames) return;
        auto& track = audio[index];
        const auto alignment = static_cast<std::size_t>(track.input.channels) * track.input.bits_per_sample / 8;
        if (alignment == 0 || frames > std::numeric_limits<std::size_t>::max() / alignment) {
            throw std::invalid_argument("WASAPI audio packet size overflow");
        }
        if (frames > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("WASAPI audio packet exceeds FFmpeg frame capacity");
        }
        const std::uint8_t* input_data[] = {data, nullptr, nullptr, nullptr};
        if (!track.input.floating_point && track.input.bits_per_sample == 24) {
            const auto samples = static_cast<std::size_t>(frames) * track.input.channels;
            track.pcm24.resize(samples);
            for (std::size_t i = 0; i < samples; ++i) {
                const auto offset = i * 3;
                std::uint32_t value = static_cast<std::uint32_t>(data[offset])
                    | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                    | (static_cast<std::uint32_t>(data[offset + 2]) << 16);
                if (value & 0x800000) value |= 0xff000000;
                track.pcm24[i] = static_cast<std::int32_t>(value << 8);
            }
            input_data[0] = reinterpret_cast<const std::uint8_t*>(track.pcm24.data());
        }
        const auto input_pts = av_rescale_q(time, capture_time_base, track.encoder->time_base);
        if (track.next_pts == AV_NOPTS_VALUE) track.next_pts = input_pts;
        if (track.encoded_pts == AV_NOPTS_VALUE) track.encoded_pts = track.next_pts;
        const int capacity = swr_get_out_samples(track.resampler, static_cast<int>(frames));
        if (capacity < 0) require_ffmpeg(capacity, "Sizing resampled audio buffer failed");
        prepare_resample_frame(track, std::max(capacity, 1));
        auto* frame = track.resample_frame;
        const int converted = swr_convert(track.resampler, frame->data, frame->nb_samples,
            input_data, static_cast<int>(frames));
        require_ffmpeg(converted, "Resampling WASAPI audio to 48 kHz stereo failed");
        if (converted) {
            frame->nb_samples = converted;
            frame->pts = track.next_pts;
            track.next_pts += converted;
            queue_audio_frame(index, frame, false);
        }
    }

    void flush_resampler(std::size_t index) {
        auto& track = audio[index];
        while (swr_get_delay(track.resampler, track.input.sample_rate) > 0) {
            const auto capacity64 = av_rescale_rnd(swr_get_delay(track.resampler, track.input.sample_rate),
                output_rate, track.input.sample_rate, AV_ROUND_UP);
            if (capacity64 <= 0 || capacity64 > std::numeric_limits<int>::max()) {
                throw std::runtime_error("FFmpeg resampler flush size is invalid");
            }
            prepare_resample_frame(track, static_cast<int>(capacity64));
            auto* frame = track.resample_frame;
            const int converted = swr_convert(track.resampler, frame->data, frame->nb_samples, nullptr, 0);
            require_ffmpeg(converted, "Flushing WASAPI audio resampler failed");
            if (!converted) break;
            frame->nb_samples = converted;
            if (track.next_pts == AV_NOPTS_VALUE) track.next_pts = 0;
            if (track.encoded_pts == AV_NOPTS_VALUE) track.encoded_pts = track.next_pts;
            frame->pts = track.next_pts;
            track.next_pts += converted;
            queue_audio_frame(index, frame, false);
        }
        encode_audio_fifo(index, true);
    }

    void finalize() {
        if (finalized) throw std::logic_error("ProRes MOV writer has already been finalized");
        if (!header_written) throw std::logic_error("ProRes MOV header is not available for finalization");
        require_ffmpeg(avcodec_send_frame(video_encoder, nullptr), "Flushing ProRes encoder failed");
        drain_encoder(video_encoder, video_stream, video_last_dts, true, true);
        if (video_packet_count != static_cast<std::uint64_t>(next_video_pts)) {
            throw std::runtime_error("ProRes encoder emitted a different number of packets than input frames");
        }
        if (audio_last_dts.size() < audio.size()) audio_last_dts.resize(audio.size(), AV_NOPTS_VALUE);
        for (std::size_t index = 0; index < audio.size(); ++index) {
            flush_resampler(index);
            auto& track = audio[index];
            require_ffmpeg(avcodec_send_frame(track.encoder, nullptr), "Flushing AAC encoder failed");
            drain_encoder(track.encoder, track.stream, audio_last_dts[index], false, true);
        }
        require_ffmpeg(av_write_trailer(format), "Writing ProRes MOV trailer failed");
        if (format->pb) require_ffmpeg(avio_closep(&format->pb), "Closing ProRes MOV output failed");
        finalized = true;
    }
};

FFmpegProResWriter::FFmpegProResWriter(ID3D11Device* device, const WriterOptions& options)
    : impl_(std::make_unique<Impl>(device, options)) {}
FFmpegProResWriter::~FFmpegProResWriter() = default;
void FFmpegProResWriter::write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration) {
    impl_->write_video(bgra, time, duration);
}
void FFmpegProResWriter::write_audio(std::size_t track, const std::uint8_t* data,
                                     std::uint32_t frames, std::int64_t time) {
    impl_->write_audio(track, data, frames, time);
}
void FFmpegProResWriter::finalize() { impl_->finalize(); }
std::string FFmpegProResWriter::encoder_name() const { return impl_->encoder; }

void ffmpeg_remux_segments(const std::vector<MediaSegment>& segments,
                           const std::filesystem::path& output) {
    if (segments.empty() || output.empty()) throw std::invalid_argument("MOV remux needs segments and output path");
    if (std::filesystem::exists(output)) throw std::runtime_error("Media output file already exists");
    auto temporary = output;
    temporary += L".dogsc-" + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(GetTickCount64()) + L".tmp";
    if (std::filesystem::exists(temporary)) throw std::runtime_error("MOV remux temporary file already exists");

    AVFormatContext* destination = nullptr;
    AVFormatContext* first = nullptr;
    AVPacket* packet = nullptr;
    bool temporary_created = false;
    try {
        const auto output_path = path_utf8(temporary);
        require_ffmpeg(avformat_alloc_output_context2(&destination, nullptr, "mov", output_path.c_str()),
            "Allocating final MOV muxer failed");
        if (!destination) throw std::runtime_error("FFmpeg did not create a final MOV context");
        const auto first_path = path_utf8(segments.front().path);
        require_ffmpeg(avformat_open_input(&first, first_path.c_str(), nullptr, nullptr), "Opening first MOV segment failed");
        require_ffmpeg(avformat_find_stream_info(first, nullptr), "Reading first MOV segment streams failed");
        if (!first->nb_streams) throw std::runtime_error("First MOV segment has no streams");
        for (unsigned i = 0; i < first->nb_streams; ++i) {
            AVStream* target = avformat_new_stream(destination, nullptr);
            if (!target) throw std::bad_alloc();
            require_ffmpeg(avcodec_parameters_copy(target->codecpar, first->streams[i]->codecpar),
                "Copying MOV segment codec parameters failed");
            copy_packet_metadata(target, first->streams[i]);
        }
        require_ffmpeg(av_dict_copy(&destination->metadata, first->metadata, 0),
            "Copying MOV metadata failed");
        if (!(destination->oformat->flags & AVFMT_NOFILE)) {
            require_ffmpeg(avio_open(&destination->pb, output_path.c_str(), AVIO_FLAG_WRITE),
                "Opening final MOV temporary output failed");
            temporary_created = true;
        }
        require_ffmpeg(avformat_write_header(destination, nullptr), "Writing final MOV header failed");
        packet = av_packet_alloc();
        if (!packet) throw std::bad_alloc();

        std::vector<std::int64_t> last_dts(destination->nb_streams, AV_NOPTS_VALUE);
        std::int64_t previous_end = 0;
        for (const auto& segment : segments) {
            if (segment.path.empty() || segment.output_start != previous_end || segment.duration <= 0) {
                throw std::invalid_argument("MOV segments must form a contiguous positive-duration sequence");
            }
            previous_end += segment.duration;
            AVFormatContext* source = nullptr;
            const auto source_path = path_utf8(segment.path);
            require_ffmpeg(avformat_open_input(&source, source_path.c_str(), nullptr, nullptr), "Opening MOV segment failed");
            try {
                require_ffmpeg(avformat_find_stream_info(source, nullptr), "Reading MOV segment streams failed");
                if (source->nb_streams != destination->nb_streams) {
                    throw std::runtime_error("MOV segment stream count differs from the first segment");
                }
                for (unsigned i = 0; i < source->nb_streams; ++i) {
                    if (!compatible_codec_parameters(first->streams[i]->codecpar, source->streams[i]->codecpar)) {
                        throw std::runtime_error("MOV segment codec configuration differs; lossless remux is unsafe");
                    }
                }
                while (true) {
                    const int read_result = av_read_frame(source, packet);
                    if (read_result == AVERROR_EOF) break;
                    require_ffmpeg(read_result, "Reading MOV segment packet failed");
                    if (packet->stream_index < 0 || static_cast<unsigned>(packet->stream_index) >= source->nb_streams) {
                        av_packet_unref(packet);
                        throw std::runtime_error("MOV segment has an invalid packet stream index");
                    }
                    const auto stream_index = static_cast<unsigned>(packet->stream_index);
                    const AVStream* input_stream = source->streams[stream_index];
                    AVStream* output_stream = destination->streams[stream_index];
                    const auto end_pts = av_rescale_q(segment.duration, capture_time_base, input_stream->time_base);
                    if (packet->pts != AV_NOPTS_VALUE && packet->pts >= end_pts) {
                        av_packet_unref(packet);
                        continue;
                    }
                    av_packet_rescale_ts(packet, input_stream->time_base, output_stream->time_base);
                    const auto offset = av_rescale_q(segment.output_start, capture_time_base, output_stream->time_base);
                    if (packet->pts != AV_NOPTS_VALUE) packet->pts += offset;
                    if (packet->dts != AV_NOPTS_VALUE) packet->dts += offset;
                    packet->stream_index = static_cast<int>(stream_index);
                    packet->pos = -1;
                    if (packet->dts != AV_NOPTS_VALUE && last_dts[stream_index] != AV_NOPTS_VALUE
                        && packet->dts < last_dts[stream_index]) {
                        av_packet_unref(packet);
                        throw std::runtime_error("MOV segment packet timestamps are not monotonic");
                    }
                    if (packet->dts != AV_NOPTS_VALUE) last_dts[stream_index] = packet->dts;
                    require_ffmpeg(av_interleaved_write_frame(destination, packet), "Writing remuxed MOV packet failed");
                    av_packet_unref(packet);
                }
            } catch (...) {
                avformat_close_input(&source);
                throw;
            }
            avformat_close_input(&source);
        }
        require_ffmpeg(av_write_trailer(destination), "Writing final MOV trailer failed");
        if (destination->pb) require_ffmpeg(avio_closep(&destination->pb), "Closing final MOV output failed");
        std::filesystem::rename(temporary, output);
        temporary_created = false;
    } catch (...) {
        if (packet) av_packet_free(&packet);
        if (first) avformat_close_input(&first);
        close_format(destination);
        if (temporary_created) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        throw;
    }
    if (packet) av_packet_free(&packet);
    if (first) avformat_close_input(&first);
    close_format(destination);
}

} // namespace dogsc::capture
