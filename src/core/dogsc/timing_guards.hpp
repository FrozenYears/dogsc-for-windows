#pragma once

#include <optional>
#include <vector>

#include "dogsc/core_types.hpp"

namespace dogsc {

/// 录制会话的三条时序契约。它们都是「不可事后修复」的：
/// 违反后只能在成片里看到丢帧、音画漂移或时间轴跳变，
/// 因此把判定抽成纯函数，供采集层与导出层共用同一套护栏。
class TimingGuards {
public:
    /// 契约一：首帧 host-time 归一。
    ///
    /// 所有辅助轨（摄像头 / 麦克风 / 指针）都以屏幕首帧的 host-time 为
    /// 对齐基准。`session_start` 一旦确定就不得改变；后续帧按
    /// `host_time - session_start - accumulated_pause` 归一化。
    /// 返回归一化后的时间；参数非法时返回空。
    static std::optional<TimeInterval> normalize_frame_time(TimeInterval host_time,
                                                            TimeInterval session_start,
                                                            TimeInterval accumulated_pause);

    /// 契约二：非单调 PTS 必须丢弃，而不是交给编码器。
    ///
    /// 屏幕采集在显示器切换 / 时钟重同步后会短暂回退 PTS；
    /// 直接写入会让编码器永久失败并终止整段录制。
    static bool should_discard_non_monotonic(TimeInterval candidate,
                                             std::optional<TimeInterval> last_accepted);

    /// 契约三：停止时冻结共享 stop host-time。
    ///
    /// 摄像头 / 麦克风可能比屏幕晚落盘，但它们在工程里的引用必须
    /// 截断到同一个 host 边界；否则后录的那一路会带进时间轴之外的尾巴。
    /// 返回该路源时间；host 时间早于该路起点时返回空（应记为 0）。
    static std::optional<TimeInterval> source_time_at_shared_stop(
        TimeInterval shared_stop_host_time, TimeInterval source_start_host_time,
        TimeInterval source_clock_offset = 0);

    /// 暂停补偿：恢复时把「暂停期间本应产生的帧间隔」计入累计暂停。
    /// 只有确实超过一帧间隔的中断才算暂停，否则会把正常抖动当暂停。
    static TimeInterval pause_interruption(TimeInterval resume_host_time,
                                           TimeInterval pause_start_host_time,
                                           int frame_rate);
};

}  // namespace dogsc
