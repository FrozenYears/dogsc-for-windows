#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace dogsc::capture {

// 保留区间为端开区间；所有轨使用同一个首帧原点、暂停边界和停止边界。
class RecordingClock {
public:
    struct Span {
        std::int64_t host_start;
        std::optional<std::int64_t> host_end;
        std::int64_t output_start;
    };
    struct AudioSlice {
        std::uint32_t first_frame;
        std::uint32_t frame_count;
        std::int64_t output_time;
    };

    void start(std::int64_t host) {
        if (!spans_.empty()) throw std::logic_error("录制时钟已启动");
        spans_.push_back({host, std::nullopt, 0});
    }
    void pause(std::int64_t host) {
        if (spans_.empty() || stopped_ || paused_) throw std::logic_error("录制时钟不能暂停");
        close_span(host);
        paused_ = true;
    }
    void resume(std::int64_t host) {
        if (!paused_ || stopped_) throw std::logic_error("录制时钟不能恢复");
        if (host < *spans_.back().host_end) throw std::invalid_argument("恢复时间倒退");
        spans_.push_back({host, std::nullopt, duration_});
        paused_ = false;
    }
    void stop(std::int64_t host) {
        if (spans_.empty() || stopped_) throw std::logic_error("录制时钟不能停止");
        if (paused_) {
            if (host < *spans_.back().host_end) throw std::invalid_argument("停止时间倒退");
        } else {
            close_span(host);
        }
        stopped_ = true;
    }
    bool paused() const { return paused_; }
    bool started() const { return !spans_.empty(); }
    std::int64_t elapsed(std::int64_t now) const {
        if (spans_.empty()) return 0;
        if (paused_ || stopped_) return duration_;
        const auto& span = spans_.back();
        return span.output_start + std::max<std::int64_t>(now - span.host_start, 0);
    }
    std::optional<std::int64_t> map(std::int64_t host) const {
        for (auto it = spans_.rbegin(); it != spans_.rend(); ++it) {
            if (host >= it->host_start && (!it->host_end || host < *it->host_end)) {
                return it->output_start + host - it->host_start;
            }
        }
        return std::nullopt;
    }
    std::vector<AudioSlice> slice_audio(std::int64_t host, std::uint32_t frames,
                                      std::uint32_t rate,
                                      std::optional<std::int64_t> output_limit = std::nullopt) const {
        if (rate == 0) throw std::invalid_argument("音频采样率为零");
        std::vector<AudioSlice> result;
        // ceil(delta*rate/10^7)：不保留跨越首帧/暂停/停止边界的样本。
        const auto index_at = [&](std::int64_t boundary) {
            if (boundary <= host) return std::uint32_t{0};
            const auto delta = boundary - host;
            const auto packet_duration =
                (static_cast<std::int64_t>(frames) * 10000000 + rate - 1) / rate;
            if (delta >= packet_duration) return frames;
            return static_cast<std::uint32_t>(std::min<std::int64_t>(frames,
                (delta * rate + 9999999) / 10000000));
        };
        for (const auto& span : spans_) {
            auto host_end = span.host_end;
            if (output_limit) {
                if (span.output_start >= *output_limit) break;
                const auto limit = span.host_start + *output_limit - span.output_start;
                if (!host_end || limit < *host_end) host_end = limit;
            }
            const auto first = index_at(span.host_start);
            auto end = host_end ? index_at(*host_end) : frames;
            // end 是首个起点不小于边界的样本；完整样本尾部也不得跨界。
            if (host_end && end > first &&
                host + (static_cast<std::int64_t>(end) * 10000000 + rate - 1) / rate >
                    *host_end) --end;
            if (end > first) {
                const auto retained_host = host + static_cast<std::int64_t>(first) * 10000000 / rate;
                result.push_back({first, end - first,
                    span.output_start + retained_host - span.host_start});
            }
        }
        return result;
    }
    const std::vector<Span>& spans() const { return spans_; }

private:
    void close_span(std::int64_t host) {
        auto& span = spans_.back();
        if (host < span.host_start) throw std::invalid_argument("录制时间倒退");
        span.host_end = host;
        duration_ = span.output_start + host - span.host_start;
    }
    std::vector<Span> spans_;
    std::int64_t duration_ = 0;
    bool paused_ = false;
    bool stopped_ = false;
};
} // namespace dogsc::capture
