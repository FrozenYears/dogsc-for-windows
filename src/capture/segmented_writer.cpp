#include "dogsc/segmented_writer.hpp"
#include "dogsc/media_remux.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <nlohmann/json.hpp>
#include <winrt/base.h>

namespace dogsc::capture {
namespace {
using json = nlohmann::ordered_json;
constexpr std::int64_t segment_ticks = 100000000;
constexpr std::int64_t second_ticks = 10000000;

void save_journal(const std::filesystem::path& directory, const json& value) {
    const auto temporary = directory / "segments.next";
    const auto destination = directory / "segments.json";
    std::ofstream output(temporary, std::ios::binary);
    output << value.dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("录制分段日志写入失败");
    winrt::check_bool(MoveFileExW(temporary.c_str(), destination.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}
}

struct SegmentedWriter::Impl {
    struct Segment {
        std::unique_ptr<MediaWriter> writer;
        MediaSegment media;
        bool finalized = false;
    };
    winrt::com_ptr<ID3D11Device> device;
    WriterOptions options;
    std::filesystem::path output;
    std::filesystem::path directory;
    std::vector<Segment> segments;
    std::vector<std::int64_t> audio_progress;
    std::int64_t video_progress = 0;
    bool has_video = false;
    bool finished = false;
    std::size_t next_to_finish = 0;
    std::string encoder;
    bool hardware = false;

    Impl(ID3D11Device* input_device, WriterOptions requested)
        : options(std::move(requested)), output(options.path),
          directory(output.parent_path() / (output.stem().wstring() + L".parts")),
          audio_progress(options.audio_formats.size(), 0),
          has_video(options.width != 0 || options.height != 0) {
        if (std::filesystem::exists(output)) throw std::runtime_error("媒体输出文件已存在");
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("录制分段目录已存在");
        device.copy_from(input_device);
        options.fragmented = false;
        try {
            auto& first = segment(0);
            encoder = first.writer->video_encoder_name();
            hardware = first.writer->video_encoder_is_hardware();
            journal();
        } catch (...) {
            segments.clear();
            // 仅回滚本构造函数刚创建、尚无完成分段的目录，绝不覆盖已有工程。
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
            throw;
        }
    }
    void journal() const {
        json entries = json::array();
        for (const auto& segment : segments) {
            entries.push_back({{"file", winrt::to_string(segment.media.path.filename().wstring())},
                {"outputStart100ns", segment.media.output_start},
                {"duration100ns", segment.media.duration}, {"finalized", segment.finalized}});
        }
        save_journal(directory, json{{"version", 1}, {"segmentDuration100ns", segment_ticks},
            {"output", winrt::to_string(output.filename().wstring())}, {"segments", std::move(entries)}});
    }
    Segment& segment(std::size_t index) {
        if (finished) throw std::logic_error("录制已完成");
        if (index > segments.size()) throw std::invalid_argument("分段时间戳存在空洞");
        if (index == segments.size()) {
            std::wostringstream name;
            name << L"part-" << std::setfill(L'0') << std::setw(6) << index << L".mp4";
            auto settings = options;
            settings.path = (directory / name.str()).wstring();
            auto writer = std::make_unique<MediaWriter>(device.get(), settings);
            segments.push_back({std::move(writer),
                {directory / name.str(), static_cast<std::int64_t>(index) * segment_ticks, segment_ticks}, false});
            journal();
        }
        if (segments[index].finalized) throw std::invalid_argument("样本时间回退到已完成分段");
        return segments[index];
    }
    void finish_ready() {
        while (next_to_finish < segments.size()) {
            auto& current = segments[next_to_finish];
            const auto boundary = current.media.output_start + segment_ticks;
            if (has_video && video_progress < boundary) break;
            if (std::any_of(audio_progress.begin(), audio_progress.end(),
                [boundary](auto time) { return time < boundary; })) break;
            current.writer->finalize();
            current.writer.reset();
            current.finalized = true;
            ++next_to_finish;
            journal();
        }
    }
};

SegmentedWriter::SegmentedWriter(ID3D11Device* device, WriterOptions options)
    : impl_(std::make_unique<Impl>(device, std::move(options))) {}
SegmentedWriter::~SegmentedWriter() = default;
std::string SegmentedWriter::video_encoder_name() const { return impl_->encoder; }
bool SegmentedWriter::video_encoder_is_hardware() const { return impl_->hardware; }
void SegmentedWriter::write_video(ID3D11Texture2D* bgra, std::int64_t time, std::int64_t duration) {
    auto& state = *impl_;
    if (!state.has_video || time < state.video_progress || duration <= 0) {
        throw std::invalid_argument("分段视频时间戳无效或倒退");
    }
    while (duration > 0) {
        const auto index = static_cast<std::size_t>(time / segment_ticks);
        auto& segment = state.segment(index);
        const auto count = std::min(duration, segment_ticks - (time % segment_ticks));
        segment.writer->write_video(bgra, time - segment.media.output_start, count);
        time += count;
        duration -= count;
        state.video_progress = time;
        state.finish_ready();
    }
}
void SegmentedWriter::write_audio(std::size_t track, const std::uint8_t* data,
                                  std::uint32_t frames, std::int64_t time) {
    auto& state = *impl_;
    if (track >= state.options.audio_formats.size() || time < 0 || (frames && !data)) {
        throw std::invalid_argument("分段音频输入无效");
    }
    const auto& format = state.options.audio_formats[track];
    const auto alignment = static_cast<std::size_t>(format.channels) * format.bits_per_sample / 8;
    auto sample = (time / second_ticks) * format.sample_rate +
        ((time % second_ticks) * format.sample_rate + second_ticks / 2) / second_ticks;
    while (frames > 0) {
        const auto index = static_cast<std::size_t>(sample / (static_cast<std::int64_t>(format.sample_rate) * 10));
        auto& segment = state.segment(index);
        const auto end_sample = static_cast<std::int64_t>(index + 1) * format.sample_rate * 10;
        const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(frames, end_sample - sample));
        const auto sample_time = sample * second_ticks / format.sample_rate;
        segment.writer->write_audio(track, data, count, sample_time - segment.media.output_start);
        sample += count;
        frames -= count;
        data += static_cast<std::size_t>(count) * alignment;
        state.audio_progress[track] = sample * second_ticks / format.sample_rate;
        state.finish_ready();
    }
}
void SegmentedWriter::finalize(std::int64_t duration) {
    auto& state = *impl_;
    if (state.finished) throw std::logic_error("分段录制已完成");
    if (duration <= 0) throw std::invalid_argument("录制时长无效");
    std::vector<MediaSegment> media;
    for (auto& segment : state.segments) {
        const auto retained = std::min(segment_ticks, duration - segment.media.output_start);
        if (retained <= 0) throw std::logic_error("录制产生了停止边界之外的分段");
        if (!segment.finalized) {
            segment.writer->finalize();
            segment.writer.reset();
            segment.finalized = true;
        }
        segment.media.duration = retained;
        media.push_back(segment.media);
        state.journal();
    }
    remux_segments(media, state.output);
    state.finished = true;
}
void recover_segments(const std::filesystem::path& directory, const std::filesystem::path& output) {
    std::ifstream input(directory / "segments.json", std::ios::binary);
    if (!input) throw std::runtime_error("找不到分段恢复日志");
    const auto journal = json::parse(input);
    if (journal.at("version").get<int>() != 1) throw std::runtime_error("不支持的分段日志版本");
    std::vector<MediaSegment> media;
    std::int64_t next = 0;
    for (const auto& entry : journal.at("segments")) {
        if (!entry.at("finalized").get<bool>()) break;
        const auto name = std::filesystem::path(winrt::to_hstring(entry.at("file").get<std::string>()).c_str());
        const auto start = entry.at("outputStart100ns").get<std::int64_t>();
        const auto duration = entry.at("duration100ns").get<std::int64_t>();
        if (name.has_parent_path() || name.is_absolute() || start != next || duration <= 0 || duration > segment_ticks) {
            throw std::runtime_error("分段日志路径或时间范围无效");
        }
        media.push_back({directory / name, start, duration});
        next += duration;
    }
    if (media.empty()) throw std::runtime_error("没有已完成且可恢复的分段");
    remux_segments(media, output);
}
} // namespace dogsc::capture
