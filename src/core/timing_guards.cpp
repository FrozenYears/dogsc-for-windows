#include "dogsc/timing_guards.hpp"

#include <algorithm>
#include <cmath>

namespace dogsc {

std::optional<TimeInterval> TimingGuards::normalize_frame_time(TimeInterval host_time,
                                                              TimeInterval session_start,
                                                              TimeInterval accumulated_pause) {
    if (!std::isfinite(host_time) || !std::isfinite(session_start) ||
        !std::isfinite(accumulated_pause)) {
        return std::nullopt;
    }
    if (accumulated_pause < 0) return std::nullopt;
    return host_time - session_start - accumulated_pause;
}

bool TimingGuards::should_discard_non_monotonic(TimeInterval candidate,
                                                std::optional<TimeInterval> last_accepted) {
    if (!std::isfinite(candidate)) return true;
    if (!last_accepted.has_value()) return false;
    // 严格小于才丢弃：相等时间戳是合法重复样本，编码器可以接受。
    return candidate < *last_accepted;
}

std::optional<TimeInterval> TimingGuards::source_time_at_shared_stop(
    TimeInterval shared_stop_host_time, TimeInterval source_start_host_time,
    TimeInterval source_clock_offset) {
    if (!std::isfinite(shared_stop_host_time) || !std::isfinite(source_start_host_time) ||
        !std::isfinite(source_clock_offset)) {
        return std::nullopt;
    }
    const TimeInterval elapsed = shared_stop_host_time - source_start_host_time;
    if (elapsed < 0) return std::nullopt;
    return elapsed + source_clock_offset;
}

TimeInterval TimingGuards::pause_interruption(TimeInterval resume_host_time,
                                              TimeInterval pause_start_host_time,
                                              int frame_rate) {
    if (!std::isfinite(resume_host_time) || !std::isfinite(pause_start_host_time) ||
        frame_rate <= 0) {
        return 0;
    }
    const TimeInterval expected_interval = 1.0 / static_cast<double>(frame_rate);
    const TimeInterval interruption =
        resume_host_time - pause_start_host_time - expected_interval;
    return interruption > 0 ? interruption : 0;
}

}  // namespace dogsc
