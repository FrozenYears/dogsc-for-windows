#include "commands.hpp"

#include <algorithm>
#include <atomic>
#include <conio.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <dwmapi.h>
#include "dogsc/camera_capture.hpp"
#include "dogsc/pointer_capture.hpp"
#include "dogsc/project_json.hpp"
#include "dogsc/recording_clock.hpp"
#include "dogsc/segmented_writer.hpp"

namespace dogsc::cli {
namespace {
using json = nlohmann::ordered_json;
std::atomic_bool stop_requested = false;
BOOL WINAPI console_control(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        stop_requested.store(true);
        return TRUE;
    }
    return FALSE;
}
struct ConsoleControl {
    ConsoleControl() { stop_requested.store(false); winrt::check_bool(SetConsoleCtrlHandler(console_control, TRUE)); }
    ~ConsoleControl() { SetConsoleCtrlHandler(console_control, FALSE); }
};
void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text << '\n';
    stream.close();
    if (!stream) throw std::runtime_error("文件写入失败: " + winrt::to_string(path.wstring()));
}
RECT pointer_bounds(const RecordOptions& options, UINT width, UINT height) {
    RECT bounds{};
    if (options.target.window) {
        if (FAILED(DwmGetWindowAttribute(options.target.window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &bounds, sizeof(bounds)))) winrt::check_bool(GetWindowRect(options.target.window, &bounds));
    } else {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        winrt::check_bool(GetMonitorInfoW(options.target.monitor, &monitor));
        bounds = monitor.rcMonitor;
    }
    bounds.left += options.target.region.left;
    bounds.top += options.target.region.top;
    bounds.right = bounds.left + static_cast<LONG>(width);
    bounds.bottom = bounds.top + static_cast<LONG>(height);
    return bounds;
}
struct AudioTrack {
    std::unique_ptr<capture::AudioCapture> source;
    capture::SegmentedWriter* writer = nullptr;
    capture::AudioFormat format;
    std::uint64_t written_frames = 0;
    std::uint64_t captured_frames = 0;
    std::uint64_t discontinuities = 0;
    std::vector<std::uint8_t> silence;

    explicit AudioTrack(capture::AudioSource kind)
        : source(std::make_unique<capture::AudioCapture>(kind)), format(source->format()),
          silence(static_cast<std::size_t>(1024) * block_align(), 0) {}
    std::size_t block_align() const { return static_cast<std::size_t>(format.channels) * format.bits_per_sample / 8; }
    std::int64_t time_at(std::uint64_t frame) const {
        return static_cast<std::int64_t>(frame * 10000000 / format.sample_rate);
    }
    void pad_to(std::uint64_t frame) {
        while (written_frames < frame) {
            const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(frame - written_frames, 1024));
            writer->write_audio(0, silence.data(), count, time_at(written_frames));
            written_frames += count;
        }
    }
    void pad_through(std::int64_t time) {
        if (time > 0) pad_to(static_cast<std::uint64_t>(time) * format.sample_rate / 10000000);
    }
    void drain(const capture::RecordingClock& clock,
               std::optional<std::int64_t> output_limit = std::nullopt) {
        for (const auto& packet : source->drain()) {
            if (packet.discontinuity) ++discontinuities;
            for (const auto& slice : clock.slice_audio(packet.host_time, packet.frames,
                                                       format.sample_rate, output_limit)) {
                auto first = slice.first_frame;
                auto count = slice.frame_count;
                const auto output_frame = static_cast<std::uint64_t>(
                    (slice.output_time * format.sample_rate + 5000000) / 10000000);
                if (output_frame < written_frames) {
                    const auto overlap = static_cast<std::uint32_t>(std::min<std::uint64_t>(written_frames - output_frame, count));
                    first += overlap;
                    count -= overlap;
                } else {
                    pad_to(output_frame);
                }
                if (count) {
                    writer->write_audio(0, packet.bytes.data() + first * block_align(), count, time_at(written_frames));
                    written_frames += count;
                    captured_frames += count;
                }
            }
        }
    }
    void finish(const capture::RecordingClock& clock, std::int64_t stop_host) {
        source->stop();
        drain(clock);
        const auto total = static_cast<std::uint64_t>(clock.elapsed(stop_host)) * format.sample_rate / 10000000;
        pad_to(total);
    }
};
}

int record(const RecordOptions& options) {
    ConsoleControl control;
    capture::ScreenCapture screen(options.target);
    if (!std::filesystem::create_directories(options.directory)) {
        throw std::runtime_error("工程目录已存在，拒绝覆盖");
    }
    json report{{"status", "preparing"}, {"width", screen.width()}, {"height", screen.height()},
        {"frameRate", options.frame_rate}, {"systemAudio", options.system_audio},
        {"microphone", options.microphone}, {"camera", options.camera}, {"cursorInVideo", false}};
    try {
        std::ofstream pointer_file(options.directory / "pointer.jsonl", std::ios::binary);
        if (!pointer_file) throw std::runtime_error("无法创建指针轨");
        // 指针钩子先于所有音视频源启动。
        capture::PointerCapture pointer;
        std::unique_ptr<capture::CameraCapture> camera;
        if (options.camera) camera = std::make_unique<capture::CameraCapture>(screen.device());
        std::int64_t last_camera_source_pts = std::numeric_limits<std::int64_t>::min();
        std::int64_t last_written_camera_source_pts = last_camera_source_pts;
        std::int64_t camera_received = 0;
        std::int64_t camera_discarded = 0;
        std::int64_t camera_repeated = 0;
        std::int64_t camera_video_frames = 0;
        std::int64_t camera_device_qpc_frames = 0;
        std::int64_t camera_arrival_timestamp_frames = 0;
        capture::ScreenFrame camera_latest;
        const auto accept_camera_frame = [&](capture::ScreenFrame frame) {
            if (!frame.texture) return;
            if (frame.host_time < last_camera_source_pts) {
                ++camera_discarded;
                return;
            }
            last_camera_source_pts = frame.host_time;
            camera_latest = std::move(frame);
            ++camera_received;
            if (camera->timestamp_is_device_qpc()) ++camera_device_qpc_frames;
            else ++camera_arrival_timestamp_frames;
        };
        std::unique_ptr<AudioTrack> system;
        std::unique_ptr<AudioTrack> microphone;
        if (options.system_audio) system = std::make_unique<AudioTrack>(capture::AudioSource::system);
        if (options.microphone) microphone = std::make_unique<AudioTrack>(capture::AudioSource::microphone);
        capture::WriterOptions writer_options;
        writer_options.path = (options.directory / "screen.mp4").wstring();
        writer_options.width = screen.width();
        writer_options.height = screen.height();
        writer_options.frame_rate = static_cast<UINT>(options.frame_rate);
        writer_options.codec = options.codec == "h264" ? capture::VideoCodec::h264 : capture::VideoCodec::hevc;
        if (system) writer_options.audio_formats.push_back(system->format);
        std::unique_ptr<capture::SegmentedWriter> video;
        try {
            video = std::make_unique<capture::SegmentedWriter>(screen.device(), writer_options);
        } catch (const winrt::hresult_error& error) {
            if (options.codec != "auto") throw;
            report["hevcInitializationError"] = winrt::to_string(error.message());
        } catch (const std::exception& error) {
            if (options.codec != "auto") throw;
            report["hevcInitializationError"] = error.what();
        }
        if (!video) {
            writer_options.codec = capture::VideoCodec::h264;
            std::cerr << "HEVC unavailable; trying H.264 (native size, no implicit downscale).\n";
            video = std::make_unique<capture::SegmentedWriter>(screen.device(), writer_options);
        }
        std::unique_ptr<capture::SegmentedWriter> camera_video;
        if (camera) {
            capture::WriterOptions camera_options;
            camera_options.path = (options.directory / "camera.mp4").wstring();
            camera_options.width = camera->width();
            camera_options.height = camera->height();
            camera_options.frame_rate = static_cast<UINT>(options.frame_rate);
            camera_options.codec = writer_options.codec;
            camera_video = std::make_unique<capture::SegmentedWriter>(screen.device(), camera_options);
            report["cameraWidth"] = camera->width();
            report["cameraHeight"] = camera->height();
            report["cameraEncoder"] = camera_video->video_encoder_name();
            report["cameraHardwareEncoder"] = camera_video->video_encoder_is_hardware();
        }
        std::unique_ptr<capture::SegmentedWriter> microphone_writer;
        if (microphone) {
            capture::WriterOptions audio_options;
            audio_options.path = (options.directory / "microphone.m4a").wstring();
            audio_options.audio_formats.push_back(microphone->format);
            microphone_writer = std::make_unique<capture::SegmentedWriter>(nullptr, audio_options);
            microphone->writer = microphone_writer.get();
        }
        if (system) system->writer = video.get();
        report["codec"] = writer_options.codec == capture::VideoCodec::hevc ? "hevc" : "h264";
        report["encoder"] = video->video_encoder_name();
        report["hardwareEncoder"] = video->video_encoder_is_hardware();
        report["containerStrategy"] = "independent-10s-mp4-then-native-remux";
        report["status"] = "recording";
        write_text(options.directory / "capture.json", report.dump(2));
        if (system) system->source->start();
        if (microphone) microphone->source->start();
        // 摄像头在指针钩子之后先启动，并在启动屏幕采集前等待真实首帧。
        if (camera) {
            camera->start();
            const auto camera_start = capture::host_time_100ns();
            while (!camera_latest.texture) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) throw std::runtime_error("指针采集消息循环异常退出");
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                while (_kbhit()) {
                    const int key = _getch();
                    if (key == 'q' || key == 'Q') stop_requested.store(true);
                }
                if (stop_requested.load()) throw std::runtime_error("摄像头首帧到达前录制已取消");
                accept_camera_frame(camera->poll());
                if (camera_latest.texture) break;
                if (capture::host_time_100ns() - camera_start >= 100000000) {
                    throw std::runtime_error("10 秒内未收到摄像头帧");
                }
                Sleep(1);
            }
        }
        screen.start();
        const auto preparing_start = capture::host_time_100ns();
        capture::RecordingClock clock;
        capture::ScreenFrame latest;
        std::int64_t first_screen_host = 0;
        std::int64_t last_source_pts = std::numeric_limits<std::int64_t>::min();
        std::int64_t last_written_source_pts = last_source_pts;
        std::int64_t frame_index = 0;
        std::int64_t discarded = 0;
        std::int64_t received = 0;
        std::int64_t repeated = 0;
        bool scheduled_pause_done = false;
        std::optional<std::int64_t> resume_at;
        std::int64_t stop_host = 0;
        std::int64_t last_pointer_drain = 0;
        auto current_pointer_bounds = pointer_bounds(options, screen.width(), screen.height());
        const auto requested_duration = static_cast<std::int64_t>(options.duration * 10000000);
        const auto video_time = [&](std::int64_t index) { return index * 10000000 / options.frame_rate; };
        std::cout << "Recording " << screen.width() << 'x' << screen.height() << '@' << options.frame_rate
                  << " with " << video->video_encoder_name() << ". P pause/resume; Q stop.\n";
        while (true) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) throw std::runtime_error("指针采集消息循环异常退出");
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            auto now = capture::host_time_100ns();
            bool toggle_pause = false;
            while (_kbhit()) {
                const int key = _getch();
                if (key == 'p' || key == 'P') toggle_pause = true;
                if (key == 'q' || key == 'Q') stop_requested.store(true);
            }
            if (screen.closed()) stop_requested.store(true);
            if (clock.started()) {
                // 到达输出时长先冻结准确边界；不能先按迟到的循环时刻暂停，
                // 否则 stop_host 会落在已经关闭的时间段之前。
                if (requested_duration > 0 && clock.elapsed(now) >= requested_duration) {
                    const auto& span = clock.spans().back();
                    stop_host = span.host_start + requested_duration - span.output_start;
                    break;
                }
                if (!scheduled_pause_done && options.pause_at >= 0 && !clock.paused() &&
                    clock.elapsed(now) >= static_cast<std::int64_t>(options.pause_at * 10000000)) {
                    toggle_pause = true;
                    scheduled_pause_done = true;
                    resume_at = now + static_cast<std::int64_t>(options.pause_for * 10000000);
                }
                if (resume_at && now >= *resume_at) { toggle_pause = clock.paused(); resume_at.reset(); }
                if (toggle_pause) {
                    if (clock.paused()) { clock.resume(now); std::cout << "Resumed\n"; }
                    else { clock.pause(now); std::cout << "Paused\n"; }
                }
                if (stop_requested.load()) { stop_host = now; break; }
            } else if (stop_requested.load()) {
                if (camera && !camera_latest.texture) throw std::runtime_error("摄像头首帧到达前录制已取消");
                throw std::runtime_error("首帧到达前录制已取消");
            }
            auto frame = screen.poll();
            if (frame.texture) {
                if (frame.host_time < last_source_pts) ++discarded;
                else {
                    if (!received) first_screen_host = frame.host_time;
                    last_source_pts = frame.host_time;
                    latest = std::move(frame);
                    ++received;
                }
            }
            if (camera) accept_camera_frame(camera->poll());
            if (!clock.started() && latest.texture && (!camera || camera_latest.texture)) {
                clock.start(first_screen_host);
            }
            if (!clock.started()) {
                if (now - preparing_start >= 100000000) {
                    if (camera && !camera_latest.texture) throw std::runtime_error("10 秒内未收到摄像头帧");
                    throw std::runtime_error("10 秒内未收到屏幕帧");
                }
                // 尚无统一原点时，仅丢弃肯定早于首帧的音频，防止准备阶段缓冲溢出。
                if (system) system->source->drain();
                if (microphone) microphone->source->drain();
                Sleep(1);
                continue;
            }
            const auto output_limit = requested_duration > 0 ?
                std::optional<std::int64_t>{requested_duration} : std::nullopt;
            if (system) system->drain(clock, output_limit);
            if (microphone) microphone->drain(clock, output_limit);
            const auto elapsed = clock.elapsed(now);
            // loopback 静音时可能没有包；保留一秒端点缓冲余量后补静音，
            // 使旧分段能及时完成，而不是长期占用多个硬件编码会话。
            const auto audio_horizon = std::min(elapsed,
                requested_duration > 0 ? requested_duration : elapsed) - 10000000;
            if (system) system->pad_through(audio_horizon);
            if (microphone) microphone->pad_through(audio_horizon);
            if (!clock.paused()) {
                while (video_time(frame_index) < elapsed || frame_index == 0) {
                    const auto time = video_time(frame_index);
                    if (requested_duration > 0 && time >= requested_duration) break;
                    auto duration = video_time(frame_index + 1) - time;
                    if (requested_duration > 0) duration = std::min(duration, requested_duration - time);
                    video->write_video(latest.texture.get(), time, duration);
                    if (last_written_source_pts == last_source_pts) ++repeated;
                    last_written_source_pts = last_source_pts;
                    if (camera_video) {
                        camera_video->write_video(camera_latest.texture.get(), time, duration);
                        if (last_written_camera_source_pts == last_camera_source_pts) ++camera_repeated;
                        last_written_camera_source_pts = last_camera_source_pts;
                        ++camera_video_frames;
                    }
                    ++frame_index;
                }
            }
            if (now - last_pointer_drain >= 10000000 / options.frame_rate) {
                if (!options.target.window || (IsWindow(options.target.window) && !IsIconic(options.target.window))) {
                    current_pointer_bounds = pointer_bounds(options, screen.width(), screen.height());
                }
                pointer.drain(clock, current_pointer_bounds, pointer_file);
                last_pointer_drain = now;
            }
            Sleep(1);
        }
        // 先冻结共享边界，再停止任何异步采集源和 writer。
        clock.stop(stop_host);
        screen.stop();
        if (camera) camera->stop();
        if (system) system->finish(clock, stop_host);
        if (microphone) microphone->finish(clock, stop_host);
        const auto duration = clock.elapsed(stop_host);
        while (video_time(frame_index) < duration) {
            const auto time = video_time(frame_index);
            const auto frame_duration = std::min(video_time(frame_index + 1), duration) - time;
            video->write_video(latest.texture.get(), time, frame_duration);
            ++frame_index;
            ++repeated;
            if (camera_video) {
                camera_video->write_video(camera_latest.texture.get(), time, frame_duration);
                if (last_written_camera_source_pts == last_camera_source_pts) ++camera_repeated;
                last_written_camera_source_pts = last_camera_source_pts;
                ++camera_video_frames;
            }
        }
        pointer.drain(clock, current_pointer_bounds, pointer_file);
        pointer_file.close();
        if (!pointer_file) throw std::runtime_error("指针轨关闭失败");
        video->finalize(duration);
        if (camera_video) camera_video->finalize(duration);
        if (microphone_writer) microphone_writer->finalize(duration);
        RecorderProject project;
        project.title = winrt::to_string(options.directory.filename().wstring());
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        char date[40]{};
        std::snprintf(date, sizeof(date), "%04u-%02u-%02uT%02u:%02u:%02uZ",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
        project.created_at = date;
        project.export_settings.frame_rate = options.frame_rate;
        project.camera.is_hidden = !camera;
        project.media = ProjectMediaManifest{};
        const auto reference = [&](std::string path) {
            ProjectMediaReference media;
            media.relative_path = std::move(path);
            media.source_end_time = static_cast<double>(duration) / 10000000.0;
            media.source_time_scale = 1;
            return media;
        };
        project.media->screen = reference("screen.mp4");
        project.media->pointer_events = reference("pointer.jsonl");
        if (camera) project.media->camera = reference("camera.mp4");
        if (microphone) project.media->microphone = reference("microphone.m4a");
        write_text(options.directory / "project.json", ProjectJson::encode(project));
        report["status"] = "finalized";
        report["duration"] = static_cast<double>(duration) / 10000000.0;
        report["firstFrameHostTime100ns"] = clock.spans().front().host_start;
        report["sharedStopHostTime100ns"] = stop_host;
        report["videoFrames"] = frame_index;
        report["capturedFrames"] = received;
        report["repeatedFrames"] = repeated;
        report["discardedNonMonotonicFrames"] = discarded;
        report["spans"] = json::array();
        for (const auto& span : clock.spans()) report["spans"].push_back({
            {"hostStart100ns", span.host_start}, {"hostEnd100ns", *span.host_end},
            {"outputStart100ns", span.output_start}});
        if (camera) {
            report["cameraTimestampMode"] = camera_arrival_timestamp_frames == 0 ? "device-qpc" :
                camera_device_qpc_frames == 0 ? "host-arrival-qpc" : "mixed";
            report["cameraDeviceQpcFrames"] = camera_device_qpc_frames;
            report["cameraHostArrivalFrames"] = camera_arrival_timestamp_frames;
            report["cameraCapturedFrames"] = camera_received;
            report["cameraVideoFrames"] = camera_video_frames;
            report["cameraRepeatedFrames"] = camera_repeated;
            report["cameraDiscardedNonMonotonicFrames"] = camera_discarded;
            if (camera_arrival_timestamp_frames) {
                report["cameraHostArrivalTimestampNote"] =
                    "Callback arrival QPC includes unknown capture/conversion latency and is not exposure time.";
            }
        }
        if (system) report["systemAudioCapturedFrames"] = system->captured_frames;
        if (system) report["systemAudioDiscontinuities"] = system->discontinuities;
        if (microphone) report["microphoneCapturedFrames"] = microphone->captured_frames;
        if (microphone) report["microphoneDiscontinuities"] = microphone->discontinuities;
        write_text(options.directory / "capture.json", report.dump(2));
        std::cout << report.dump(2) << '\n';
        return 0;
    } catch (const winrt::hresult_error& error) {
        report["status"] = "failed";
        report["error"] = winrt::to_string(error.message());
        report["hresult"] = error.code().value;
        write_text(options.directory / "capture.json", report.dump(2));
        throw;
    } catch (const std::exception& error) {
        report["status"] = "failed";
        report["error"] = error.what();
        write_text(options.directory / "capture.json", report.dump(2));
        throw;
    }
}
} // namespace dogsc::cli
