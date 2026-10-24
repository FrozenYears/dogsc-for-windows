#include "dogsc/media_remux.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <winrt/base.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace dogsc::capture {
namespace {

using winrt::check_hresult;
using winrt::com_ptr;

void check_stage(HRESULT result, const wchar_t* stage) {
    if (FAILED(result)) throw winrt::hresult_error(result, stage);
}

std::int64_t add_time(std::int64_t time, std::int64_t offset) {
    if ((offset > 0 && time > std::numeric_limits<std::int64_t>::max() - offset)
        || (offset < 0 && time < std::numeric_limits<std::int64_t>::min() - offset)) {
        throw winrt::hresult_error(E_INVALIDARG, L"分段时间戳超出 100ns 时间范围。");
    }
    return time + offset;
}

bool has_blob(IMFMediaType* type, REFGUID key) {
    UINT32 size = 0;
    const HRESULT result = type->GetBlobSize(key, &size);
    if (result == MF_E_ATTRIBUTENOTFOUND) return false;
    check_hresult(result);
    return size != 0;
}

struct AttributeValue {
    PROPVARIANT value{};
    ~AttributeValue() { PropVariantClear(&value); }
};

void require_attribute_match(IMFMediaType* expected, IMFMediaType* actual, REFGUID key) {
    AttributeValue value;
    const HRESULT result = expected->GetItem(key, &value.value);
    if (result == MF_E_ATTRIBUTENOTFOUND) {
        MF_ATTRIBUTE_TYPE type{};
        const HRESULT actual_result = actual->GetItemType(key, &type);
        if (actual_result == MF_E_ATTRIBUTENOTFOUND) return;
        check_hresult(actual_result);
    } else {
        check_hresult(result);
        BOOL equal = FALSE;
        check_hresult(actual->CompareItem(key, value.value, &equal));
        if (equal) return;
    }
    throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE,
        L"分段的编码格式或 codec 配置不一致，不能无损合并。");
}

void require_compatible(IMFMediaType* expected, IMFMediaType* actual) {
    const GUID common[] = {MF_MT_MAJOR_TYPE, MF_MT_SUBTYPE, MF_MT_USER_DATA,
        MF_MT_MPEG_SEQUENCE_HEADER, MF_MT_ORIGINAL_4CC};
    for (const auto& key : common) require_attribute_match(expected, actual, key);

    GUID major{};
    check_hresult(expected->GetGUID(MF_MT_MAJOR_TYPE, &major));
    if (major == MFMediaType_Video) {
        const GUID keys[] = {MF_MT_FRAME_SIZE, MF_MT_PIXEL_ASPECT_RATIO,
            MF_MT_INTERLACE_MODE, MF_MT_MPEG2_PROFILE, MF_MT_MPEG2_LEVEL,
            MF_MT_VIDEO_PRIMARIES, MF_MT_TRANSFER_FUNCTION, MF_MT_YUV_MATRIX,
            MF_MT_VIDEO_NOMINAL_RANGE, MF_MT_VIDEO_ROTATION, MF_MT_GEOMETRIC_APERTURE,
            MF_MT_MINIMUM_DISPLAY_APERTURE, MF_MT_PAN_SCAN_APERTURE};
        for (const auto& key : keys) require_attribute_match(expected, actual, key);
    } else {
        const GUID keys[] = {MF_MT_AUDIO_SAMPLES_PER_SECOND, MF_MT_AUDIO_NUM_CHANNELS,
            MF_MT_AUDIO_CHANNEL_MASK, MF_MT_AUDIO_BITS_PER_SAMPLE,
            MF_MT_AUDIO_BLOCK_ALIGNMENT, MF_MT_AAC_PAYLOAD_TYPE,
            MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION};
        for (const auto& key : keys) require_attribute_match(expected, actual, key);
    }
    // stsd 可以含每个文件自己的码率统计；已有独立 codec 配置时不比较这些统计。
    // 未解析出 codec 配置时则必须逐字相同，不能猜测未知的 sample entry 兼容。
    const bool has_config = major == MFMediaType_Video
        ? has_blob(expected, MF_MT_MPEG_SEQUENCE_HEADER) : has_blob(expected, MF_MT_USER_DATA);
    if (!has_config) {
        require_attribute_match(expected, actual, MF_MT_MPEG4_SAMPLE_DESCRIPTION);
    }
}

struct InputTrack {
    DWORD source_index;
    GUID major;
    com_ptr<IMFMediaType> type;
    bool ended = false;
    bool wrote_sample = false;
};

struct SegmentReader {
    com_ptr<IMFSourceReader> reader;
    std::vector<InputTrack> tracks;

    explicit SegmentReader(const std::filesystem::path& path) {
        com_ptr<IMFAttributes> attributes;
        check_hresult(MFCreateAttributes(attributes.put(), 1));
        check_hresult(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE));
        check_stage(MFCreateSourceReaderFromURL(path.c_str(), attributes.get(), reader.put()),
            L"无法打开待合并的独立 MP4 分段。");
        check_hresult(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
        for (DWORD index = 0; index < MF_SOURCE_READER_FIRST_VIDEO_STREAM; ++index) {
            com_ptr<IMFMediaType> type;
            const HRESULT result = reader->GetNativeMediaType(index, 0, type.put());
            if (result == MF_E_INVALIDSTREAMNUMBER) break;
            check_stage(result, L"无法读取分段的原始压缩媒体类型。");
            GUID major{};
            GUID subtype{};
            check_hresult(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
            if (major != MFMediaType_Video && major != MFMediaType_Audio) continue;
            check_hresult(type->GetGUID(MF_MT_SUBTYPE, &subtype));
            if ((major == MFMediaType_Video
                    && subtype != MFVideoFormat_HEVC && subtype != MFVideoFormat_H264)
                || (major == MFMediaType_Audio && subtype != MFAudioFormat_AAC)) {
                throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE,
                    L"无损 MP4 合并仅接受原始 HEVC/H.264 视频和 AAC 音频。");
            }
            // 第一个 native type 来自文件源；绝不请求 PCM/NV12 等解码后格式。
            check_hresult(reader->SetCurrentMediaType(index, nullptr, type.get()));
            check_hresult(reader->SetStreamSelection(index, TRUE));
            com_ptr<IMFMediaType> current;
            check_hresult(reader->GetCurrentMediaType(index, current.put()));
            require_compatible(type.get(), current.get());
            tracks.push_back({index, major, std::move(current)});
        }
        if (tracks.empty()) {
            throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE, L"MP4 分段没有可合并的音视频轨。");
        }
    }
};

struct OutputTrack {
    GUID major;
    com_ptr<IMFMediaType> type;
    DWORD sink_identifier = 0;
    DWORD writer_index = static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX);
};

struct RemuxOutput {
    com_ptr<IMFByteStream> bytes;
    com_ptr<IMFMediaSink> sink;
    com_ptr<IMFSinkWriter> writer;
    std::vector<OutputTrack> tracks;

    ~RemuxOutput() { close(); }

    HRESULT close() noexcept {
        // writer 持有 sink，必须先释放；Finalize 不等于 Shutdown。
        writer = nullptr;
        HRESULT result = S_OK;
        if (sink) {
            // sink 负责关闭其 byte stream，仅在创建 sink 失败时自行关闭。
            bytes = nullptr;
            result = sink->Shutdown();
            if (result == MF_E_SHUTDOWN) result = S_OK;
            sink = nullptr;
        }
        if (bytes) {
            const HRESULT closed = bytes->Close();
            if (SUCCEEDED(result) && FAILED(closed) && closed != MF_E_SHUTDOWN) result = closed;
            bytes = nullptr;
        }
        return result;
    }

    void create(const std::filesystem::path& output, const std::vector<InputTrack>& input) {
        // 原子拒绝覆盖，不能用 exists 检查后再创建留下竞争窗口。
        check_stage(MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST,
            MF_FILEFLAGS_NONE, output.c_str(), bytes.put()), L"不能创建合并输出：文件可能已存在。");
        IMFMediaType* video = nullptr;
        IMFMediaType* audio = nullptr;
        for (const auto& track : input) {
            if (track.major == MFMediaType_Video) {
                if (video) {
                    throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE,
                        L"原生 MP4 sink 只支持一个视频轨，不能丢弃额外视频轨。");
                }
                video = track.type.get();
            } else if (!audio) {
                audio = track.type.get();
            }
            tracks.push_back({track.major, track.type});
        }
        check_stage(MFCreateMPEG4MediaSink(bytes.get(), video, audio, sink.put()),
            L"原生 MP4 sink 拒绝输入分段的压缩类型。");

        DWORD count = 0;
        DWORD next_identifier = 1;
        check_hresult(sink->GetStreamSinkCount(&count));
        for (DWORD index = 0; index < count; ++index) {
            com_ptr<IMFStreamSink> stream;
            com_ptr<IMFMediaTypeHandler> handler;
            GUID major{};
            DWORD identifier = 0;
            check_hresult(sink->GetStreamSinkByIndex(index, stream.put()));
            check_hresult(stream->GetIdentifier(&identifier));
            check_hresult(stream->GetMediaTypeHandler(handler.put()));
            check_hresult(handler->GetMajorType(&major));
            if (identifier == std::numeric_limits<DWORD>::max()) {
                throw winrt::hresult_error(E_UNEXPECTED, L"MP4 sink 流标识符已溢出。");
            }
            next_identifier = std::max(next_identifier, identifier + 1);
            const auto track = std::find_if(tracks.begin(), tracks.end(), [&](const OutputTrack& item) {
                return item.major == major && item.writer_index == MF_SINK_WRITER_INVALID_STREAM_INDEX;
            });
            if (track == tracks.end()) {
                throw winrt::hresult_error(MF_E_INVALIDSTREAMNUMBER, L"MP4 sink 创建了未知媒体轨。");
            }
            track->sink_identifier = identifier;
            track->writer_index = index;
        }
        for (auto& track : tracks) {
            if (track.writer_index != MF_SINK_WRITER_INVALID_STREAM_INDEX) continue;
            if (next_identifier == std::numeric_limits<DWORD>::max()) {
                throw winrt::hresult_error(E_UNEXPECTED, L"MP4 sink 流标识符已溢出。");
            }
            com_ptr<IMFStreamSink> stream;
            check_stage(sink->AddStreamSink(next_identifier, track.type.get(), stream.put()),
                L"原生 MP4 sink 不支持全部请求的音轨，不能静默丢轨。");
            track.sink_identifier = next_identifier++;
        }

        com_ptr<IMFAttributes> attributes;
        check_hresult(MFCreateAttributes(attributes.put(), 2));
        check_hresult(attributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE));
        check_hresult(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
        check_hresult(MFCreateSinkWriterFromMediaSink(sink.get(), attributes.get(), writer.put()));
        // sink 标识符不是 SinkWriter 索引；扩展音轨之后重新按实际顺序映射。
        for (auto& track : tracks) track.writer_index = static_cast<DWORD>(MF_SINK_WRITER_INVALID_STREAM_INDEX);
        check_hresult(sink->GetStreamSinkCount(&count));
        for (DWORD index = 0; index < count; ++index) {
            com_ptr<IMFStreamSink> stream;
            DWORD identifier = 0;
            check_hresult(sink->GetStreamSinkByIndex(index, stream.put()));
            check_hresult(stream->GetIdentifier(&identifier));
            const auto track = std::find_if(tracks.begin(), tracks.end(), [&](const OutputTrack& item) {
                return item.sink_identifier == identifier;
            });
            if (track == tracks.end()) {
                throw winrt::hresult_error(MF_E_INVALIDSTREAMNUMBER, L"无法映射 MP4 sink 媒体轨。");
            }
            track->writer_index = index;
            // 同一完整压缩类型作为输入和输出，SinkWriter 不得插入编码器。
            check_stage(writer->SetInputMediaType(index, track->type.get(), nullptr),
                L"SinkWriter 拒绝压缩样本直通，不能退回重新编码。");
        }
        if (count != tracks.size()
            || std::any_of(tracks.begin(), tracks.end(), [](const OutputTrack& track) {
                return track.writer_index == MF_SINK_WRITER_INVALID_STREAM_INDEX;
            })) {
            throw winrt::hresult_error(MF_E_INVALIDSTREAMNUMBER, L"合并输出缺少请求的媒体轨。");
        }
        check_hresult(writer->BeginWriting());
    }

    void finalize() {
        const HRESULT finalized = writer->Finalize();
        const HRESULT closed = close();
        check_stage(finalized, L"原生无损合并 Finalize 失败，输出文件不完整。");
        check_stage(closed, L"原生无损合并关闭输出失败。");
    }
};

void append_segment(SegmentReader& source, const MediaSegment& segment, RemuxOutput& output) {
    if (source.tracks.size() != output.tracks.size()) {
        throw winrt::hresult_error(MF_E_INVALIDSTREAMNUMBER, L"分段媒体轨数量不一致，不能丢轨合并。");
    }
    for (std::size_t index = 0; index < source.tracks.size(); ++index) {
        require_compatible(output.tracks[index].type.get(), source.tracks[index].type.get());
    }

    std::size_t active = source.tracks.size();
    while (active != 0) {
        DWORD actual_index = 0;
        DWORD flags = 0;
        LONGLONG event_time = 0;
        com_ptr<IMFSample> sample;
        check_stage(source.reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0,
            &actual_index, &flags, &event_time, sample.put()), L"读取分段压缩样本失败。");
        if (flags & MF_SOURCE_READERF_ERROR) {
            throw winrt::hresult_error(E_FAIL, L"分段源读取器报告错误。");
        }
        if (flags & MF_SOURCE_READERF_NEWSTREAM) {
            throw winrt::hresult_error(MF_E_INVALIDSTREAMNUMBER, L"分段中途增加了媒体轨。");
        }
        const auto input = std::find_if(source.tracks.begin(), source.tracks.end(),
            [&](const InputTrack& track) { return track.source_index == actual_index; });
        if (input == source.tracks.end() || input->ended) {
            throw winrt::hresult_error(MF_E_INVALIDSTREAMNUMBER, L"分段源返回了未选择的媒体轨。");
        }
        const auto index = static_cast<std::size_t>(input - source.tracks.begin());
        const auto& target = output.tracks[index];
        if (flags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED
                | MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) {
            com_ptr<IMFMediaType> current;
            check_hresult(source.reader->GetCurrentMediaType(actual_index, current.put()));
            require_compatible(target.type.get(), current.get());
        }
        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            if (event_time >= 0 && event_time < segment.duration) {
                check_hresult(output.writer->SendStreamTick(target.writer_index,
                    add_time(event_time, segment.output_start)));
            }
        }
        if (sample) {
            LONGLONG time = 0;
            LONGLONG duration = 0;
            check_hresult(sample->GetSampleTime(&time));
            check_stage(sample->GetSampleDuration(&duration), L"压缩样本缺少时长，无法精确裁定分段尾部。");
            if (time < 0 || duration <= 0) {
                throw winrt::hresult_error(MF_E_INVALID_TIMESTAMP, L"分段压缩样本不是有效的局部零起点时间轴。");
            }
            if (time < segment.duration) {
                if (!input->wrote_sample && input->major == MFMediaType_Video) {
                    UINT32 clean = FALSE;
                    check_stage(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean),
                        L"独立视频分段的首样本未标识为关键帧。");
                    if (!clean) {
                        throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE,
                            L"独立视频分段未从关键帧开始，不能无损拼接。");
                    }
                }
                // 尾包跨过边界仍须保留压缩载荷，只裁展示时长；不能整包丢掉有效尾部。
                check_hresult(sample->SetSampleDuration(std::min(duration, segment.duration - time)));
                check_hresult(sample->SetSampleTime(add_time(time, segment.output_start)));
                UINT64 decode = 0;
                const HRESULT decode_result = sample->GetUINT64(MFSampleExtension_DecodeTimestamp, &decode);
                if (decode_result != MF_E_ATTRIBUTENOTFOUND) {
                    check_hresult(decode_result);
                    const auto decode_time = static_cast<std::int64_t>(decode);
                    check_hresult(sample->SetUINT64(MFSampleExtension_DecodeTimestamp,
                        static_cast<UINT64>(add_time(decode_time, segment.output_start))));
                }
                if (!input->wrote_sample) {
                    check_hresult(sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE));
                }
                check_stage(output.writer->WriteSample(target.writer_index, sample.get()),
                    L"原生 MP4 sink 写入压缩样本失败。");
                input->wrote_sample = true;
            }
        }
        // 必须读完所有轨，不能看到一个轨 EOS 或越过 duration 就停止读其它轨。
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            input->ended = true;
            --active;
            check_hresult(source.reader->SetStreamSelection(actual_index, FALSE));
        }
    }
    for (const auto& track : source.tracks) {
        if (!track.wrote_sample) {
            throw winrt::hresult_error(MF_E_INVALIDMEDIATYPE, L"分段请求的媒体轨在有效区间内没有样本。");
        }
    }
}

} // namespace

void remux_segments(const std::vector<MediaSegment>& segments, const std::filesystem::path& output) {
    if (segments.empty() || output.empty()) {
        throw winrt::hresult_error(E_INVALIDARG, L"无损合并需要分段和输出路径。");
    }
    std::int64_t previous_end = 0;
    for (const auto& segment : segments) {
        if (segment.path.empty() || segment.output_start < previous_end || segment.duration <= 0) {
            throw winrt::hresult_error(E_INVALIDARG, L"分段路径、顺序或时长无效。");
        }
        previous_end = add_time(segment.output_start, segment.duration);
    }

    SegmentReader first(segments.front().path);
    RemuxOutput target;
    target.create(output, first.tracks);
    append_segment(first, segments.front(), target);
    first.reader = nullptr;
    for (std::size_t index = 1; index < segments.size(); ++index) {
        SegmentReader source(segments[index].path);
        append_segment(source, segments[index], target);
    }
    target.finalize();
}

} // namespace dogsc::capture
