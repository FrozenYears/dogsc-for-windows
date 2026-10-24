#include "dogsc/native_capture.hpp"

#include <codecapi.h>
#include <d3d10.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <winrt/base.h>

#include "dogsc/ffmpeg_prores.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace dogsc::capture {
namespace {

using winrt::check_hresult;
using winrt::com_ptr;
constexpr std::int64_t ticks_per_second = 10'000'000;
constexpr GUID output_view_key{
    0x49965a3d, 0x4c24, 0x4d6c, {0x95, 0x3e, 0x81, 0xaf, 0xa7, 0xd1, 0x83, 0x67}};

void check_stage(HRESULT result, const wchar_t* stage) {
    if (FAILED(result)) throw winrt::hresult_error(result, stage);
}

struct ActivationList {
    IMFActivate** values = nullptr;
    UINT32 count = 0;

    ~ActivationList() {
        for (UINT32 index = 0; index < count; ++index) {
            values[index]->Release();
        }
        CoTaskMemFree(values);
    }
};

struct TaskMemoryDeleter {
    void operator()(wchar_t* value) const noexcept { CoTaskMemFree(value); }
};

std::wstring attribute_string(IMFAttributes* attributes, REFGUID key) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    const HRESULT result = attributes->GetAllocatedString(key, &value, &length);
    std::unique_ptr<wchar_t, TaskMemoryDeleter> owner(value);
    if (result == MF_E_ATTRIBUTENOTFOUND) {
        return {};
    }
    check_hresult(result);
    return std::wstring(value, length);
}

std::string guid_string(REFGUID value) {
    wchar_t text[39]{};
    if (!StringFromGUID2(value, text, static_cast<int>(std::size(text)))) {
        throw winrt::hresult_error(E_UNEXPECTED, L"无法格式化编码器 CLSID。");
    }
    return winrt::to_string(text);
}

const GUID& codec_subtype(VideoCodec codec) {
    return codec == VideoCodec::hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
}

UINT32 video_bitrate(const WriterOptions& options) {
    const auto pixels = static_cast<std::uint64_t>(options.width) * options.height;
    const std::uint64_t ceiling = pixels < 3'500'000 ? 65'000'000
        : pixels < 7'000'000 ? 100'000'000 : 130'000'000;
    // 先与上限比较，避免无效的超大参数使乘法溢出；结果仍是原公式的精确整数值。
    const auto rate = pixels > ceiling * 6 / options.frame_rate
        ? ceiling : pixels * options.frame_rate / 6;
    return static_cast<UINT32>(std::clamp(rate, std::uint64_t{12'000'000}, ceiling));
}

com_ptr<IMFMediaType> video_type(const WriterOptions& options, REFGUID subtype) {
    com_ptr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(type.put()));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    check_hresult(type->SetGUID(MF_MT_SUBTYPE, subtype));
    check_hresult(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, options.width, options.height));
    check_hresult(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, options.frame_rate, 1));
    check_hresult(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    check_hresult(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    check_hresult(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    check_hresult(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    check_hresult(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    return type;
}

UINT32 audio_block_alignment(const AudioFormat& format) {
    if (!format.sample_rate || !format.channels || !format.bits_per_sample
        || format.bits_per_sample % 8 != 0
        || (format.floating_point && format.bits_per_sample != 32 && format.bits_per_sample != 64)
        || (!format.floating_point && format.bits_per_sample != 16
            && format.bits_per_sample != 24 && format.bits_per_sample != 32)) {
        throw winrt::hresult_error(E_INVALIDARG, L"无效的 PCM/浮点音频输入格式。");
    }
    const auto alignment = static_cast<UINT32>(format.channels) * (format.bits_per_sample / 8);
    if (static_cast<std::uint64_t>(alignment) * format.sample_rate
        > std::numeric_limits<UINT32>::max()) {
        throw winrt::hresult_error(E_INVALIDARG, L"音频输入字节率超出 Media Foundation 范围。");
    }
    return alignment;
}

com_ptr<IMFMediaType> audio_input_type(const AudioFormat& format, UINT32 alignment) {
    com_ptr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(type.put()));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check_hresult(type->SetGUID(MF_MT_SUBTYPE,
        format.floating_point ? MFAudioFormat_Float : MFAudioFormat_PCM));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, format.sample_rate));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, format.channels));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, format.bits_per_sample));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, alignment));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, alignment * format.sample_rate));
    check_hresult(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    if (format.channel_mask) {
        check_hresult(type->SetUINT32(MF_MT_AUDIO_CHANNEL_MASK, format.channel_mask));
    }
    return type;
}

com_ptr<IMFMediaType> aac_output_type() {
    com_ptr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(type.put()));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check_hresult(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    check_hresult(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192'000 / 8));
    check_hresult(type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0));
    return type;
}

struct BufferLock {
    IMFMediaBuffer* buffer;
    BYTE* bytes = nullptr;

    explicit BufferLock(IMFMediaBuffer* value) : buffer(value) {
        check_hresult(buffer->Lock(&bytes, nullptr, nullptr));
    }
    ~BufferLock() { buffer->Unlock(); }
};

struct ContextLock {
    ID3D10Multithread* multithread;
    explicit ContextLock(ID3D10Multithread* value) : multithread(value) { multithread->Enter(); }
    ~ContextLock() { multithread->Leave(); }
};

struct TextureMap {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;
    D3D11_MAPPED_SUBRESOURCE mapped{};

    TextureMap(ID3D11DeviceContext* value, ID3D11Texture2D* resource)
        : context(value), texture(resource) {
        check_hresult(context->Map(texture, 0, D3D11_MAP_READ, 0, &mapped));
    }
    ~TextureMap() { context->Unmap(texture, 0); }
};

struct VariantValue {
    VARIANT value{};
    ~VariantValue() { VariantClear(&value); }
};

bool documented_nvidia_encoder(IMFAttributes* attributes, VideoCodec codec) {
    UINT32 length = 0;
    const HRESULT hardware = attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length);
    if (hardware == MF_E_ATTRIBUTENOTFOUND) {
        return false;
    }
    check_hresult(hardware);
    const auto is_nvidia = [](const std::wstring& vendor) {
        return CompareStringOrdinal(vendor.c_str(), -1, L"VEN_10DE", -1, TRUE) == CSTR_EQUAL;
    };
    const auto vendor = attribute_string(attributes, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute);
    if (!vendor.empty()) {
        return is_nvidia(vendor);
    }

    // 厂商属性是可选项；仅按实际 MFT 的硬件 URL/CLSID 匹配 activation，不按名称猜测。
    const auto hardware_url = attribute_string(attributes, MFT_ENUM_HARDWARE_URL_Attribute);
    GUID clsid{};
    const bool has_clsid = SUCCEEDED(attributes->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid));
    if (hardware_url.empty() && !has_clsid) {
        return false;
    }
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, codec_subtype(codec)};
    ActivationList list;
    check_hresult(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &output, &list.values, &list.count));
    for (UINT32 index = 0; index < list.count; ++index) {
        GUID candidate{};
        const bool matches = !hardware_url.empty()
            ? attribute_string(list.values[index], MFT_ENUM_HARDWARE_URL_Attribute) == hardware_url
            : SUCCEEDED(list.values[index]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &candidate))
                && candidate == clsid;
        if (matches && is_nvidia(attribute_string(
            list.values[index], MFT_ENUM_HARDWARE_VENDOR_ID_Attribute))) {
            return true;
        }
    }
    return false;
}

void require_no_b_frames(IMFTransform* transform, IMFAttributes* attributes,
    VideoCodec codec, bool configure) {
    com_ptr<ICodecAPI> api;
    const HRESULT queried = transform->QueryInterface(__uuidof(ICodecAPI), api.put_void());
    if (FAILED(queried) && queried != E_NOINTERFACE) {
        check_hresult(queried);
    }
    const HRESULT supported = api ? api->IsSupported(&CODECAPI_AVEncMPVDefaultBPictureCount) : E_NOTIMPL;
    if (supported != S_OK) {
        if (supported != S_FALSE && supported != E_NOTIMPL) {
            check_hresult(supported);
        }
        // NVIDIA 官方说明 H.264/HEVC MFT 接口不支持生成 B 帧，因此故意不暴露此属性。
        // 只接受真实硬件 MFT 的 VEN_10DE 证据；其它厂商或身份不明仍然严格拒绝。
        // https://forums.developer.nvidia.com/t/nvidia-h-264-and-hevc-mft-dont-expose-avencmpvdefaultbpicturecount/181439/2
        if ((supported == S_FALSE || supported == E_NOTIMPL)
            && documented_nvidia_encoder(attributes, codec)) {
            return;
        }
        throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE,
            L"实际视频编码器不支持验证零 B 帧，且没有已知的无 B 帧硬件契约。");
    }
    VariantValue current;
    HRESULT result = api->GetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &current.value);
    if (configure && (FAILED(result) || current.value.vt != VT_UI4 || current.value.ulVal != 0)) {
        VARIANT zero{};
        zero.vt = VT_UI4;
        zero.ulVal = 0;
        check_hresult(api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &zero));
        VariantClear(&current.value);
        result = api->GetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &current.value);
    }
    if (FAILED(result) || current.value.vt != VT_UI4 || current.value.ulVal != 0) {
        throw winrt::hresult_error(FAILED(result) ? result : MF_E_INVALIDMEDIATYPE,
            L"实际视频编码器未接受零 B 帧配置。");
    }
}

com_ptr<IMFMediaType> encoder_output_type(IMFTransform* transform) {
    DWORD input_count = 0;
    DWORD output_count = 0;
    check_hresult(transform->GetStreamCount(&input_count, &output_count));
    if (!output_count) {
        throw winrt::hresult_error(E_UNEXPECTED, L"视频编码器没有输出流。");
    }
    std::vector<DWORD> inputs(input_count);
    std::vector<DWORD> outputs(output_count);
    const HRESULT result = transform->GetStreamIDs(input_count, inputs.data(), output_count, outputs.data());
    if (result != E_NOTIMPL) {
        check_hresult(result);
    }
    com_ptr<IMFMediaType> type;
    check_hresult(transform->GetOutputCurrentType(result == E_NOTIMPL ? 0 : outputs.front(), type.put()));
    return type;
}

std::string encoder_name(IMFAttributes* attributes, VideoCodec codec) {
    auto name = attribute_string(attributes, MFT_FRIENDLY_NAME_Attribute);
    if (!name.empty()) {
        return winrt::to_string(name);
    }
    const auto hardware_url = attribute_string(attributes, MFT_ENUM_HARDWARE_URL_Attribute);
    GUID clsid{};
    const bool has_clsid = SUCCEEDED(attributes->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid));
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, codec_subtype(codec)};
    ActivationList list;
    check_hresult(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE
            | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &output, &list.values, &list.count));
    for (UINT32 index = 0; index < list.count; ++index) {
        GUID candidate{};
        const bool matches = !hardware_url.empty()
            ? attribute_string(list.values[index], MFT_ENUM_HARDWARE_URL_Attribute) == hardware_url
            : has_clsid && SUCCEEDED(list.values[index]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &candidate))
                && candidate == clsid;
        if (matches) {
            name = attribute_string(list.values[index], MFT_FRIENDLY_NAME_Attribute);
            if (!name.empty()) {
                return winrt::to_string(name);
            }
        }
    }
    const std::string prefix = codec == VideoCodec::hevc ? "HEVC MFT" : "H.264 MFT";
    return prefix + (has_clsid ? " " + guid_string(clsid) : " (friendly name unavailable)");
}

} // namespace

std::vector<EncoderInfo> enumerate_video_encoders() {
    std::vector<EncoderInfo> result;
    for (const auto codec : {VideoCodec::hevc, VideoCodec::h264}) {
        for (const bool hardware : {true, false}) {
            MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, codec_subtype(codec)};
            ActivationList list;
            const UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE
                : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT)
                | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
            check_hresult(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr,
                &output, &list.values, &list.count));
            for (UINT32 index = 0; index < list.count; ++index) {
                EncoderInfo info;
                info.codec = codec;
                info.hardware = hardware;
                auto name = attribute_string(list.values[index], MFT_FRIENDLY_NAME_Attribute);
                if (!name.empty()) {
                    info.name = winrt::to_string(name);
                } else {
                    GUID clsid{};
                    check_hresult(list.values[index]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid));
                    info.name = guid_string(clsid);
                }
                com_ptr<IMFTransform> transform;
                const HRESULT activated = list.values[index]->ActivateObject(
                    __uuidof(IMFTransform), transform.put_void());
                info.activation_succeeded = SUCCEEDED(activated);
                info.activation_hresult = static_cast<std::int32_t>(activated);
                if (SUCCEEDED(activated)) list.values[index]->ShutdownObject();
                result.push_back(std::move(info));
            }
        }
    }
    EncoderInfo prores;
    prores.codec = VideoCodec::prores422;
    try {
        prores.name = "FFmpeg " + ffmpeg_prores_encoder_name() + " (ProRes 422 Standard)";
        prores.activation_succeeded = true;
    } catch (const std::exception& error) {
        prores.name = error.what();
        prores.activation_hresult = static_cast<std::int32_t>(E_FAIL);
    }
    result.push_back(std::move(prores));
    return result;
}

struct MediaWriter::Impl {
    struct AudioTrack {
        DWORD stream = static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX);
        UINT32 rate = 0;
        UINT32 alignment = 0;
    };

    WriterOptions options;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<ID3D10Multithread> multithread;
    com_ptr<ID3D11VideoDevice> video_device;
    com_ptr<ID3D11VideoContext> video_context;
    com_ptr<ID3D11VideoProcessorEnumerator> processor_enum;
    com_ptr<ID3D11VideoProcessor> processor;
    com_ptr<ID3D11Texture2D> compatible_input;
    com_ptr<ID3D11Texture2D> readback;
    com_ptr<IMFDXGIDeviceManager> manager;
    com_ptr<IMFVideoSampleAllocatorEx> allocator;
    com_ptr<IMFByteStream> byte_stream;
    com_ptr<IMFMediaSink> sink;
    com_ptr<IMFSinkWriter> writer;
    std::unique_ptr<FFmpegProResWriter> prores;
    std::vector<AudioTrack> audio;
    DWORD video_stream = static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX);
    std::string encoder;
    bool hardware = false;
    bool dxgi_input = false;
    bool started = false;
    bool finalization_attempted = false;
    HRESULT finalization_result = S_OK;
    const wchar_t* finalization_stage = L"MF SinkWriter Finalize 失败。";
    UINT frame_index = 0;

    ~Impl() {
        if (prores) {
            if (!finalization_attempted) {
                try { prores->finalize(); }
                catch (...) { OutputDebugStringW(L"dogsc: FFmpeg ProRes writer destructor finalization failed.\n"); }
            }
            return;
        }
        if (started && !finalization_attempted) {
            finish();
            if (FAILED(finalization_result)) {
                OutputDebugStringW(L"dogsc: MediaWriter 析构时 Finalize 失败；正常路径必须显式调用 finalize。\n");
            }
        } else {
            close_output();
        }
        if (allocator) allocator->UninitializeSampleAllocator();
    }

    void close_output() noexcept {
        writer = nullptr;
        if (sink) {
            // sink 接管并关闭 byte stream；不能再次关闭同一文件句柄。
            byte_stream = nullptr;
            const HRESULT result = sink->Shutdown();
            if (SUCCEEDED(finalization_result) && FAILED(result)) {
                finalization_result = result;
                finalization_stage = L"MF media sink Shutdown 失败。";
            }
            sink = nullptr;
        }
        if (byte_stream) {
            const HRESULT result = byte_stream->Close();
            if (SUCCEEDED(finalization_result) && FAILED(result)) {
                finalization_result = result;
                finalization_stage = L"MF byte stream Close 失败。";
            }
            byte_stream = nullptr;
        }
    }

    void finish() noexcept {
        finalization_attempted = true;
        finalization_result = writer->Finalize();
        close_output();
    }

    void initialize(ID3D11Device* input_device, const WriterOptions& requested) {
        options = requested;
        const bool has_video = options.width != 0 || options.height != 0;
        if (options.path.empty() || (!has_video && options.audio_formats.empty())) {
            throw winrt::hresult_error(E_INVALIDARG, L"输出路径和至少一个媒体流是必需的。");
        }
        if (has_video && (!input_device || !options.width || !options.height || !options.frame_rate
            || (options.codec != VideoCodec::prores422 &&
                (options.width % 2 != 0 || options.height % 2 != 0))
            || options.width > static_cast<UINT32>(std::numeric_limits<LONG>::max())
            || options.height > static_cast<UINT32>(std::numeric_limits<LONG>::max()))) {
            throw winrt::hresult_error(E_INVALIDARG,
                options.codec == VideoCodec::prores422
                    ? L"视频需要 D3D11 设备、非零帧率和有效原生尺寸。"
                    : L"视频需要 D3D11 设备、非零帧率及偶数原生尺寸。");
        }
        if (has_video && options.codec == VideoCodec::prores422) {
            prores = std::make_unique<FFmpegProResWriter>(input_device, options);
            encoder = prores->encoder_name() + " (ProRes 422 Standard)";
            started = true;
            return;
        }
        if (has_video && options.codec == VideoCodec::h264
            && (options.width > 4096 || options.height > 2304)) {
            throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE, L"H.264 原生录制最大允许 4096x2304；请使用 HEVC。");
        }

        com_ptr<IMFAttributes> attributes;
        check_hresult(MFCreateAttributes(attributes.put(), 4));
        check_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
        check_hresult(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
        if (has_video) {
            device.copy_from(input_device);
            device->GetImmediateContext(context.put());
            multithread = context.as<ID3D10Multithread>();
            multithread->SetMultithreadProtected(TRUE);
            video_device = device.as<ID3D11VideoDevice>();
            video_context = context.as<ID3D11VideoContext>();
            UINT token = 0;
            check_hresult(MFCreateDXGIDeviceManager(&token, manager.put()));
            check_hresult(manager->ResetDevice(device.get(), token));
            check_hresult(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager.get()));
        }
        initialize_sink(attributes.get(), has_video);

        com_ptr<IMFMediaType> input_type;
        if (has_video) {
            input_type = video_type(options, MFVideoFormat_NV12);
            check_hresult(input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, options.width));
            com_ptr<IMFAttributes> parameters;
            check_hresult(MFCreateAttributes(parameters.put(), 3));
            // 在 sink writer 为编码器协商输出类型之前提交，之后再读取真实 ICodecAPI 验证。
            check_hresult(parameters->SetUINT32(CODECAPI_AVEncMPVDefaultBPictureCount, 0));
            check_hresult(parameters->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, video_bitrate(options)));
            check_hresult(parameters->SetUINT32(CODECAPI_AVEncMPVGOPSize,
                static_cast<UINT32>(std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(options.frame_rate) * 2, std::numeric_limits<UINT32>::max()))));
            check_hresult(writer->SetInputMediaType(video_stream, input_type.get(), parameters.get()));
            inspect_encoder(true);
            initialize_processor(input_type.get());
        }
        for (std::size_t index = 0; index < audio.size(); ++index) {
            const auto input = audio_input_type(options.audio_formats[index], audio[index].alignment);
            // sink writer 插入重采样及声道转换器，不把 WASAPI 混音格式谎报为 48kHz 双声道。
            check_hresult(writer->SetInputMediaType(audio[index].stream, input.get(), nullptr));
        }
        check_hresult(writer->BeginWriting());
        started = true;
        if (has_video) {
            inspect_encoder(false);
        }
    }

    void initialize_sink(IMFAttributes* attributes, bool has_video) {
        com_ptr<IMFMediaType> video_output;
        if (has_video) {
            video_output = video_type(options, codec_subtype(options.codec));
            check_hresult(video_output->SetUINT32(MF_MT_AVG_BITRATE, video_bitrate(options)));
            check_hresult(video_output->SetUINT32(MF_MT_MPEG2_PROFILE,
                options.codec == VideoCodec::hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High));
        }
        audio.reserve(options.audio_formats.size());
        for (const auto& format : options.audio_formats) {
            audio.push_back({static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX),
                format.sample_rate, audio_block_alignment(format)});
        }
        com_ptr<IMFMediaType> audio_output;
        if (!audio.empty()) {
            audio_output = aac_output_type();
        }
        check_hresult(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST,
            MF_FILEFLAGS_NONE, options.path.c_str(), byte_stream.put()));
        if (options.fragmented) {
            check_hresult(MFCreateFMPEG4MediaSink(byte_stream.get(), video_output.get(),
                audio_output.get(), sink.put()));
            const auto sink_attributes = sink.as<IMFAttributes>();
            // 属性只应用于真正的 fragmented sink。
            check_hresult(sink_attributes->SetUINT64(MF_MPEG4SINK_MIN_FRAGMENT_DURATION,
                static_cast<UINT64>(10 * ticks_per_second)));
        } else {
            check_hresult(MFCreateMPEG4MediaSink(byte_stream.get(), video_output.get(),
                audio_output.get(), sink.put()));
        }

        std::vector<DWORD> audio_identifiers(audio.size());
        DWORD count = 0;
        DWORD next_identifier = 1;
        check_hresult(sink->GetStreamSinkCount(&count));
        for (DWORD index = 0; index < count; ++index) {
            com_ptr<IMFStreamSink> stream;
            com_ptr<IMFMediaTypeHandler> handler;
            DWORD identifier = 0;
            GUID major{};
            check_hresult(sink->GetStreamSinkByIndex(index, stream.put()));
            check_hresult(stream->GetIdentifier(&identifier));
            check_hresult(stream->GetMediaTypeHandler(handler.put()));
            check_hresult(handler->GetMajorType(&major));
            if (identifier == std::numeric_limits<DWORD>::max()) {
                throw winrt::hresult_error(E_UNEXPECTED, L"媒体 sink 返回了无法扩展的流标识符。");
            }
            next_identifier = std::max(next_identifier, identifier + 1);
            if (major == MFMediaType_Audio && !audio.empty()) {
                audio_identifiers.front() = identifier;
            }
        }
        for (std::size_t index = 1; index < audio.size(); ++index) {
            com_ptr<IMFStreamSink> stream;
            // 是否可扩展由真实 sink 决定；不支持时返回原始 HRESULT，绝不丢弃请求的音轨。
            check_hresult(sink->AddStreamSink(next_identifier, audio_output.get(), stream.put()));
            audio_identifiers[index] = next_identifier++;
        }
        check_hresult(MFCreateSinkWriterFromMediaSink(sink.get(), attributes, writer.put()));
        check_hresult(sink->GetStreamSinkCount(&count));
        for (DWORD index = 0; index < count; ++index) {
            com_ptr<IMFStreamSink> stream;
            com_ptr<IMFMediaTypeHandler> handler;
            DWORD identifier = 0;
            GUID major{};
            check_hresult(sink->GetStreamSinkByIndex(index, stream.put()));
            check_hresult(stream->GetIdentifier(&identifier));
            check_hresult(stream->GetMediaTypeHandler(handler.put()));
            check_hresult(handler->GetMajorType(&major));
            if (major == MFMediaType_Video) {
                video_stream = index;
            } else if (major == MFMediaType_Audio) {
                for (std::size_t track = 0; track < audio.size(); ++track) {
                    if (audio_identifiers[track] == identifier) {
                        audio[track].stream = index;
                    }
                }
            }
        }
        if ((has_video && video_stream == MF_SINK_WRITER_INVALID_STREAM_INDEX)
            || std::any_of(audio.begin(), audio.end(), [](const AudioTrack& track) {
                return track.stream == MF_SINK_WRITER_INVALID_STREAM_INDEX;
            })) {
            throw winrt::hresult_error(MF_E_INVALIDSTREAMNUMBER, L"fragmented MP4 sink 未创建全部请求的媒体流。");
        }
    }

    void inspect_encoder(bool configure) {
        const auto extended = writer.as<IMFSinkWriterEx>();
        for (DWORD index = 0;; ++index) {
            GUID category{};
            com_ptr<IMFTransform> transform;
            const HRESULT result = extended->GetTransformForStream(video_stream, index, &category, transform.put());
            if (result == MF_E_INVALIDINDEX) {
                throw winrt::hresult_error(MF_E_TOPO_CODEC_NOT_FOUND, L"sink writer 中没有可检查的视频编码器。");
            }
            check_hresult(result);
            if (category != MFT_CATEGORY_VIDEO_ENCODER) {
                continue;
            }
            com_ptr<IMFAttributes> attributes;
            check_hresult(transform->GetAttributes(attributes.put()));
            UINT32 length = 0;
            const HRESULT url_result = attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length);
            if (url_result != MF_E_ATTRIBUTENOTFOUND) {
                check_hresult(url_result);
            }
            hardware = SUCCEEDED(url_result);
            dxgi_input = MFGetAttributeUINT32(attributes.get(), MF_SA_D3D11_AWARE, FALSE) != FALSE;
            encoder = encoder_name(attributes.get(), options.codec);
            require_no_b_frames(transform.get(), attributes.get(), options.codec, configure);
            const auto output = encoder_output_type(transform.get());
            GUID subtype{};
            UINT32 width = 0;
            UINT32 height = 0;
            UINT32 bitrate = 0;
            check_hresult(output->GetGUID(MF_MT_SUBTYPE, &subtype));
            check_hresult(MFGetAttributeSize(output.get(), MF_MT_FRAME_SIZE, &width, &height));
            check_hresult(output->GetUINT32(MF_MT_AVG_BITRATE, &bitrate));
            if (subtype != codec_subtype(options.codec) || width != options.width
                || height != options.height || bitrate != video_bitrate(options)) {
                throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE,
                    L"实际编码器协商的编码格式、原生尺寸或目标码率与请求不符。");
            }
            return;
        }
    }

    void initialize_processor(IMFMediaType* input_type) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {options.frame_rate, 1};
        content.OutputFrameRate = content.InputFrameRate;
        content.InputWidth = content.OutputWidth = options.width;
        content.InputHeight = content.OutputHeight = options.height;
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        check_hresult(video_device->CreateVideoProcessorEnumerator(&content, processor_enum.put()));
        UINT input_support = 0;
        UINT output_support = 0;
        check_hresult(processor_enum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &input_support));
        check_hresult(processor_enum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &output_support));
        if (!(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
            || !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            throw winrt::hresult_error(MF_E_UNSUPPORTED_D3D_TYPE, L"GPU 不支持 BGRA 到 NV12 视频处理。");
        }
        check_hresult(video_device->CreateVideoProcessor(processor_enum.get(), 0, processor.put()));
        const RECT rectangle{0, 0, static_cast<LONG>(options.width), static_cast<LONG>(options.height)};
        {
            ContextLock lock(multithread.get());
            video_context->VideoProcessorSetStreamFrameFormat(processor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            video_context->VideoProcessorSetStreamAutoProcessingMode(processor.get(), 0, FALSE);
            video_context->VideoProcessorSetStreamSourceRect(processor.get(), 0, TRUE, &rectangle);
            video_context->VideoProcessorSetStreamDestRect(processor.get(), 0, TRUE, &rectangle);
            video_context->VideoProcessorSetOutputTargetRect(processor.get(), TRUE, &rectangle);
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
            rgb.Usage = 1;
            rgb.YCbCr_Matrix = 1;
            rgb.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
            yuv.Usage = 1;
            yuv.YCbCr_Matrix = 1;
            yuv.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            video_context->VideoProcessorSetStreamColorSpace(processor.get(), 0, &rgb);
            video_context->VideoProcessorSetOutputColorSpace(processor.get(), &yuv);
        }
        check_hresult(MFCreateVideoSampleAllocatorEx(__uuidof(IMFVideoSampleAllocatorEx), allocator.put_void()));
        check_hresult(allocator->SetDirectXManager(manager.get()));
        com_ptr<IMFAttributes> attributes;
        check_hresult(MFCreateAttributes(attributes.put(), 2));
        check_hresult(attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_RENDER_TARGET));
        check_hresult(attributes->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT));
        check_hresult(allocator->InitializeSampleAllocatorEx(2, 16, attributes.get(), input_type));
        if (!dxgi_input) {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = options.width;
            description.Height = options.height;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_NV12;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_STAGING;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check_hresult(device->CreateTexture2D(&description, nullptr, readback.put()));
        }
    }

    com_ptr<IMFSample> gpu_sample() {
        com_ptr<IMFSample> sample;
        const HRESULT result = allocator->AllocateSample(sample.put());
        if (result != MF_E_SAMPLEALLOCATOR_EMPTY) {
            check_hresult(result);
            return sample;
        }
        // 队列暂时占满池时使用独立资源，不能覆盖编码器仍在持有的纹理，也不能阻塞其它音轨写入。
        D3D11_TEXTURE2D_DESC description{};
        description.Width = options.width;
        description.Height = options.height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_NV12;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(device->CreateTexture2D(&description, nullptr, texture.put()));
        com_ptr<IMFMediaBuffer> buffer;
        check_hresult(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.get(), 0, FALSE, buffer.put()));
        check_hresult(MFCreateSample(sample.put()));
        check_hresult(sample->AddBuffer(buffer.get()));
        return sample;
    }

    void write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration) {
        require_open();
        if (prores) {
            prores->write_video(bgra, time, duration);
            return;
        }
        if (video_stream == MF_SINK_WRITER_INVALID_STREAM_INDEX || !bgra || time < 0 || duration <= 0) {
            throw winrt::hresult_error(E_INVALIDARG, L"无效的视频帧或时间戳。");
        }
        D3D11_TEXTURE2D_DESC source_description{};
        bgra->GetDesc(&source_description);
        com_ptr<ID3D11Device> source_device;
        bgra->GetDevice(source_device.put());
        if (source_device != device || source_description.Format != DXGI_FORMAT_B8G8R8A8_UNORM
            || source_description.Width < options.width || source_description.Height < options.height
            || source_description.SampleDesc.Count != 1 || source_description.ArraySize != 1) {
            throw winrt::hresult_error(E_INVALIDARG, L"视频输入必须是同一设备上足够大的非多采样 BGRA 纹理。");
        }
        auto sample = gpu_sample();
        com_ptr<IMFMediaBuffer> buffer;
        check_hresult(sample->GetBufferByIndex(0, buffer.put()));
        DWORD contiguous_length = 0;
        check_stage(buffer.as<IMF2DBuffer>()->GetContiguousLength(&contiguous_length),
            L"读取 NV12 GPU 缓冲连续长度失败。");
        check_stage(buffer->SetCurrentLength(contiguous_length), L"设置 NV12 GPU 缓冲长度失败。");
        const auto dxgi = buffer.as<IMFDXGIBuffer>();
        com_ptr<ID3D11Texture2D> texture;
        check_hresult(dxgi->GetResource(__uuidof(ID3D11Texture2D), texture.put_void()));
        UINT subresource = 0;
        check_hresult(dxgi->GetSubresourceIndex(&subresource));
        com_ptr<ID3D11VideoProcessorOutputView> output;
        const HRESULT cached = sample->GetUnknown(output_view_key,
            __uuidof(ID3D11VideoProcessorOutputView), output.put_void());
        if (cached == MF_E_ATTRIBUTENOTFOUND) {
            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
            if (description.ArraySize > 1) {
                view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipSlice = subresource % description.MipLevels;
                view.Texture2DArray.FirstArraySlice = subresource / description.MipLevels;
                view.Texture2DArray.ArraySize = 1;
            } else {
                view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                view.Texture2D.MipSlice = subresource;
            }
            check_stage(video_device->CreateVideoProcessorOutputView(texture.get(), processor_enum.get(), &view, output.put()),
                L"创建 NV12 视频处理输出视图失败。");
            check_hresult(sample->SetUnknown(output_view_key, output.get()));
        } else {
            check_hresult(cached);
        }
        com_ptr<ID3D11VideoProcessorInputView> input;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        const UINT acceptable = D3D11_BIND_DECODER | D3D11_BIND_VIDEO_ENCODER
            | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
        const bool direct = source_description.Usage == D3D11_USAGE_DEFAULT
            && (source_description.BindFlags == 0 || (source_description.BindFlags & acceptable));
        if (!direct && !compatible_input) {
            auto description = source_description;
            description.Width = options.width;
            description.Height = options.height;
            description.MipLevels = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_RENDER_TARGET;
            description.CPUAccessFlags = description.MiscFlags = 0;
            check_hresult(device->CreateTexture2D(&description, nullptr, compatible_input.put()));
        }
        check_stage(video_device->CreateVideoProcessorInputView(direct ? bgra : compatible_input.get(),
            processor_enum.get(), &view, input.put()), L"创建 BGRA 视频处理输入视图失败。");
        {
            ContextLock lock(multithread.get());
            if (!direct) {
                const D3D11_BOX region{0, 0, 0, options.width, options.height, 1};
                context->CopySubresourceRegion(compatible_input.get(), 0, 0, 0, 0, bgra, 0, &region);
            }
            D3D11_VIDEO_PROCESSOR_STREAM stream{};
            stream.Enable = TRUE;
            stream.pInputSurface = input.get();
            check_stage(video_context->VideoProcessorBlt(processor.get(), output.get(), frame_index++, 1, &stream),
                L"GPU BGRA 到 NV12 转换失败。");
            if (!dxgi_input) {
                // 只有实际 MFT 不声明 D3D11 感知时才回读；颜色转换始终由 GPU 完成。
                const D3D11_BOX region{0, 0, 0, options.width, options.height, 1};
                context->CopySubresourceRegion(readback.get(), 0, 0, 0, 0, texture.get(), subresource, &region);
                const auto size = static_cast<DWORD>(static_cast<std::uint64_t>(options.width) * options.height * 3 / 2);
                com_ptr<IMFMediaBuffer> linear;
                check_hresult(MFCreateMemoryBuffer(size, linear.put()));
                {
                    TextureMap mapped(context.get(), readback.get());
                    BufferLock destination(linear.get());
                    const auto* source = static_cast<const BYTE*>(mapped.mapped.pData);
                    for (UINT row = 0; row < options.height + options.height / 2; ++row) {
                        std::memcpy(destination.bytes + static_cast<std::size_t>(row) * options.width,
                            source + static_cast<std::size_t>(row) * mapped.mapped.RowPitch, options.width);
                    }
                }
                check_hresult(linear->SetCurrentLength(size));
                // 用独立 CPU sample 交给软件 MFT，GPU 池中的 sample 可立即归还。
                sample = nullptr;
                check_hresult(MFCreateSample(sample.put()));
                check_hresult(sample->AddBuffer(linear.get()));
            }
            if (dxgi_input) {
                // 将视频处理命令提交给驱动，再把纹理交给编码器工作线程。
                context->Flush();
            }
        }
        check_hresult(sample->SetSampleTime(time));
        check_hresult(sample->SetSampleDuration(duration));
        check_stage(writer->WriteSample(video_stream, sample.get()), L"编码器写入视频帧失败。");
    }

    void write_audio(std::size_t track_index, const std::uint8_t* data, UINT32 frames, std::int64_t time) {
        require_open();
        if (prores) {
            prores->write_audio(track_index, data, frames, time);
            return;
        }
        if (track_index >= audio.size() || time < 0 || (frames && !data)) {
            throw winrt::hresult_error(E_INVALIDARG, L"无效的音轨、音频数据或时间戳。");
        }
        if (!frames) {
            return;
        }
        const auto& track = audio[track_index];
        const auto byte_count = static_cast<std::uint64_t>(frames) * track.alignment;
        const auto duration = static_cast<std::int64_t>(frames) * ticks_per_second / track.rate;
        if (byte_count > std::numeric_limits<DWORD>::max() || duration <= 0) {
            throw winrt::hresult_error(E_INVALIDARG, L"音频包长度或时长超出支持范围。");
        }
        com_ptr<IMFMediaBuffer> buffer;
        check_hresult(MFCreateMemoryBuffer(static_cast<DWORD>(byte_count), buffer.put()));
        {
            BufferLock destination(buffer.get());
            std::memcpy(destination.bytes, data, static_cast<std::size_t>(byte_count));
        }
        check_hresult(buffer->SetCurrentLength(static_cast<DWORD>(byte_count)));
        com_ptr<IMFSample> sample;
        check_hresult(MFCreateSample(sample.put()));
        check_hresult(sample->AddBuffer(buffer.get()));
        // 时间已由主录制时钟统一归零，保留第一包音频相对于屏幕的真实偏移。
        check_hresult(sample->SetSampleTime(time));
        check_hresult(sample->SetSampleDuration(duration));
        check_hresult(writer->WriteSample(track.stream, sample.get()));
    }

    void require_open() const {
        if (!started || finalization_attempted) {
            throw winrt::hresult_error(MF_E_INVALIDREQUEST, L"媒体写入器未启动或已经完成封装。");
        }
    }

    void finalize() {
        if (prores) {
            require_open();
            finalization_attempted = true;
            prores->finalize();
            return;
        }
        if (!finalization_attempted) {
            require_open();
            finish();
        }
        // 失败也只执行一次；再次调用返回相同错误，不重复关闭或写入 moov。
        check_stage(finalization_result, finalization_stage);
    }
};

MediaWriter::MediaWriter(ID3D11Device* device, const WriterOptions& options)
    : impl_(std::make_unique<Impl>()) {
    impl_->initialize(device, options);
}

MediaWriter::~MediaWriter() = default;

void MediaWriter::write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration) {
    impl_->write_video(bgra, time, duration);
}

void MediaWriter::write_audio(std::size_t track, const std::uint8_t* data, std::uint32_t frames,
    std::int64_t time) {
    impl_->write_audio(track, data, frames, time);
}

void MediaWriter::finalize() { impl_->finalize(); }
std::string MediaWriter::video_encoder_name() const { return impl_->encoder; }
bool MediaWriter::video_encoder_is_hardware() const { return impl_->hardware; }

} // namespace dogsc::capture
