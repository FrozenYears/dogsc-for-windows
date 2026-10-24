#include "commands.hpp"
#include "dogsc/segmented_writer.hpp"

#include <climits>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <mfapi.h>
#include <winrt/base.h>

namespace {
struct Runtime {
    Runtime() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try { winrt::check_hresult(MFStartup(MF_VERSION)); }
        catch (...) { winrt::uninit_apartment(); throw; }
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ~Runtime() { MFShutdown(); winrt::uninit_apartment(); }
};
void usage() {
    std::cout <<
        "DogSC Windows native capture\n"
        "  dogsc_capture list\n"
        "  dogsc_capture encoders\n"
        "  dogsc_capture probe <new-output-directory>\n"
        "  dogsc_capture record <new-project-directory> [options]\n"
        "  dogsc_capture recover <parts-directory> <new-media-file>\n"
        "Options:\n"
        "  --monitor <index> | --window <HWND decimal or 0xhex>\n"
        "  --region <x,y,width,height> (pixels relative to capture item)\n"
        "  --fps <30|60|90|120> --duration <active seconds>\n"
        "  --codec <auto|hevc|h264> --system-audio --microphone --camera\n"
        "  --pause-at <active seconds> --pause-for <wall seconds>\n"
        "Keys: P pause/resume, Q or Ctrl+C finalize and stop.\n"
        "Audio is opt-in. Raw screen excludes cursor; pointer.jsonl stores it separately.\n";
}
std::uint64_t integer(const std::wstring& value) {
    if (value.empty() || value.front() == L'-') throw std::invalid_argument("整数参数无效");
    std::size_t consumed = 0;
    auto result = std::stoull(value, &consumed, 0);
    if (consumed != value.size()) throw std::invalid_argument("整数参数包含无效字符");
    return result;
}
double real(const std::wstring& value) {
    std::size_t consumed = 0;
    auto result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result) ||
        std::abs(result) >= static_cast<double>(std::numeric_limits<std::int64_t>::max() / 10000000)) {
        throw std::invalid_argument("秒数参数无效");
    }
    return result;
}
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (argc < 2 || std::wstring_view(argv[1]) == L"--help") { usage(); return 0; }
        Runtime runtime;
        const std::wstring_view command(argv[1]);
        if (command == L"list" && argc == 2) { dogsc::cli::list_targets(); return 0; }
        if (command == L"encoders" && argc == 2) { dogsc::cli::list_encoders(); return 0; }
        if (command == L"probe" && argc == 3) return dogsc::cli::probe(argv[2]);
        if (command == L"recover" && argc == 4) {
            dogsc::capture::recover_segments(argv[2], argv[3]);
            std::cout << "Recovered completed segments without re-encoding.\n";
            return 0;
        }
        if (command != L"record" || argc < 3) { usage(); return 2; }
        dogsc::cli::RecordOptions options;
        options.directory = argv[2];
        std::uint64_t monitor_index = 0;
        bool has_monitor = false;
        for (int index = 3; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            const auto value = [&]() -> std::wstring {
                if (++index >= argc) throw std::invalid_argument("参数缺少值");
                return argv[index];
            };
            if (argument == L"--monitor") { monitor_index = integer(value()); has_monitor = true; }
            else if (argument == L"--window") {
                options.target.window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(integer(value())));
                if (!IsWindow(options.target.window)) throw std::invalid_argument("窗口句柄无效");
            }
            else if (argument == L"--region") {
                auto text = value();
                for (auto& c : text) if (c == L',') c = L' ';
                std::wistringstream input(text);
                long long x, y, width, height;
                if (!(input >> x >> y >> width >> height) || (input >> text) || x < 0 || y < 0 ||
                    x > LONG_MAX || y > LONG_MAX || width < 2 || height < 2 ||
                    width > LONG_MAX - x || height > LONG_MAX - y) {
                    throw std::invalid_argument("区域参数应为 x,y,width,height");
                }
                options.target.region = {static_cast<LONG>(x), static_cast<LONG>(y),
                    static_cast<LONG>(x + width), static_cast<LONG>(y + height)};
            }
            else if (argument == L"--fps") {
                const auto fps = integer(value());
                if (fps != 30 && fps != 60 && fps != 90 && fps != 120) {
                    throw std::invalid_argument("帧率仅支持 30/60/90/120");
                }
                options.frame_rate = static_cast<int>(fps);
            }
            else if (argument == L"--duration") {
                options.duration = real(value());
                if (options.duration < 0.0000001) throw std::invalid_argument("录制时长必须至少为 100ns");
            }
            else if (argument == L"--codec") {
                options.codec = winrt::to_string(value());
                if (options.codec != "auto" && options.codec != "hevc" && options.codec != "h264") {
                    throw std::invalid_argument("编码格式仅支持 auto/hevc/h264");
                }
            }
            else if (argument == L"--pause-at") {
                options.pause_at = real(value());
                if (options.pause_at < 0) throw std::invalid_argument("暂停位置不能为负数");
            }
            else if (argument == L"--pause-for") {
                options.pause_for = real(value());
                if (options.pause_for < 0.0000001) throw std::invalid_argument("暂停时长必须至少为 100ns");
            }
            else if (argument == L"--system-audio") options.system_audio = true;
            else if (argument == L"--microphone") options.microphone = true;
            else if (argument == L"--camera") options.camera = true;
            else throw std::invalid_argument("未知参数: " + winrt::to_string(argument));
        }
        if ((options.pause_at >= 0) != (options.pause_for > 0)) {
            throw std::invalid_argument("--pause-at 与 --pause-for 必须同时指定");
        }
        if (options.target.window && has_monitor) throw std::invalid_argument("不能同时选择窗口和显示器");
        if (!options.target.window) {
            struct Monitors { std::uint64_t wanted; std::uint64_t index = 0; HMONITOR found = nullptr; };
            Monitors monitors{monitor_index};
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                auto& state = *reinterpret_cast<Monitors*>(data);
                if (state.index++ == state.wanted) state.found = monitor;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&monitors));
            if (!monitors.found) throw std::invalid_argument("显示器索引不存在，先运行 list");
            options.target.monitor = monitors.found;
        }
        return dogsc::cli::record(options);
    } catch (const winrt::hresult_error& error) {
        std::cerr << "Windows API error 0x" << std::hex << static_cast<std::uint32_t>(error.code().value)
                  << ": " << winrt::to_string(error.message()) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
