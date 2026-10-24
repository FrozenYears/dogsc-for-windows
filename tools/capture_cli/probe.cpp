#include "commands.hpp"
#include "dogsc/segmented_writer.hpp"

#include <fstream>
#include <iostream>
#include <vector>
#include <dwmapi.h>
#include <nlohmann/json.hpp>
#include <winrt/base.h>

namespace dogsc::cli {
using json = nlohmann::ordered_json;
namespace {
const char* codec_name(capture::VideoCodec codec) {
    switch (codec) {
    case capture::VideoCodec::hevc: return "hevc";
    case capture::VideoCodec::h264: return "h264";
    case capture::VideoCodec::prores422: return "prores422";
    }
    return "unknown";
}
json encoders() {
    json result = json::array();
    for (const auto& encoder : capture::enumerate_video_encoders()) {
        result.push_back({{"name", encoder.name}, {"codec", codec_name(encoder.codec)},
            {"hardware", encoder.hardware}, {"activationSucceeded", encoder.activation_succeeded},
            {"activationHresult", encoder.activation_hresult}});
    }
    return result;
}
}
void list_encoders() { std::cout << encoders().dump(2) << '\n'; }
void list_targets() {
    json result{{"monitors", json::array()}, {"windows", json::array()}};
    struct State { json* array; std::exception_ptr error; } monitors{&result["monitors"], {}};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<State*>(data);
        try {
            MONITORINFOEXW information{};
            information.cbSize = sizeof(information);
            winrt::check_bool(GetMonitorInfoW(monitor, &information));
            const auto& bounds = information.rcMonitor;
            state.array->push_back({{"index", state.array->size()},
                {"device", winrt::to_string(information.szDevice)},
                {"x", bounds.left}, {"y", bounds.top},
                {"width", bounds.right - bounds.left}, {"height", bounds.bottom - bounds.top},
                {"primary", (information.dwFlags & MONITORINFOF_PRIMARY) != 0}});
            return TRUE;
        } catch (...) { state.error = std::current_exception(); return FALSE; }
    }, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.error) std::rethrow_exception(monitors.error);
    State windows{&result["windows"], {}};
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<State*>(data);
        try {
            if (!IsWindowVisible(window) || GetWindowTextLengthW(window) == 0) return TRUE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked) return TRUE;
            std::wstring title(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
            const auto count = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
            title.resize(static_cast<std::size_t>(std::max(count, 0)));
            state.array->push_back({{"hwnd", reinterpret_cast<std::uintptr_t>(window)},
                {"title", winrt::to_string(title)}, {"minimized", IsIconic(window) != FALSE}});
            return TRUE;
        } catch (...) { state.error = std::current_exception(); return FALSE; }
    }, reinterpret_cast<LPARAM>(&windows));
    if (windows.error) std::rethrow_exception(windows.error);
    std::cout << result.dump(2) << '\n';
}
int probe(const std::filesystem::path& directory) {
    if (!std::filesystem::create_directories(directory)) {
        throw std::runtime_error("探针输出目录已存在，拒绝覆盖");
    }
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
    constexpr UINT width = 1280, height = 720, frames = 30;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device->CreateTexture2D(&description, nullptr, texture.put()));
    json report{{"encoders", encoders()}, {"encodingTrials", json::array()}};
    bool encoded = false;
    struct Trial { capture::VideoCodec codec; bool fragmented; };
    for (const auto attempt : {Trial{capture::VideoCodec::hevc, true},
                              Trial{capture::VideoCodec::h264, true},
                              Trial{capture::VideoCodec::hevc, false},
                              Trial{capture::VideoCodec::h264, false},
                              Trial{capture::VideoCodec::prores422, false}}) {
        const auto codec = attempt.codec;
        const auto container = codec == capture::VideoCodec::prores422 ? "mov"
            : attempt.fragmented ? "fragmented" : "standard";
        json trial{{"codec", codec_name(codec)}, {"container", container},
            {"width", width}, {"height", height},
            {"frameRate", 30}, {"frameCount", frames}, {"succeeded", false}};
        trial["stage"] = "initialization";
        trial["submittedFrames"] = 0;
        try {
            capture::WriterOptions options;
            const auto extension = codec == capture::VideoCodec::prores422 ? ".mov" : ".mp4";
            options.path = (directory / (std::string(codec_name(codec)) + "-" + container + extension)).wstring();
            options.fragmented = attempt.fragmented;
            options.width = width;
            options.height = height;
            options.frame_rate = 30;
            options.codec = codec;
            const auto submit_frames = [&](auto& writer) {
                trial["stage"] = "encoding";
                for (UINT frame = 0; frame < frames; ++frame) {
                    for (UINT y = 0; y < height; ++y) {
                        for (UINT x = 0; x < width; ++x) {
                            pixels[static_cast<std::size_t>(y) * width + x] = 0xff000000U |
                                (((x + frame * 8) % 256) << 16) | ((y % 256) << 8) | (frame * 7 % 256);
                        }
                    }
                    context->UpdateSubresource(texture.get(), 0, nullptr, pixels.data(), width * 4, 0);
                    const auto time = static_cast<std::int64_t>(frame) * 10000000 / 30;
                    const auto end = static_cast<std::int64_t>(frame + 1) * 10000000 / 30;
                    writer.write_video(texture.get(), time, end - time);
                    trial["submittedFrames"] = frame + 1;
                }
            };
            if (codec == capture::VideoCodec::prores422) {
                capture::SegmentedWriter writer(device.get(), options);
                trial["selectedEncoder"] = writer.video_encoder_name();
                trial["selectedHardware"] = writer.video_encoder_is_hardware();
                submit_frames(writer);
                trial["stage"] = "finalization-and-remux";
                writer.finalize(10'000'000);
            } else {
                capture::MediaWriter writer(device.get(), options);
                trial["selectedEncoder"] = writer.video_encoder_name();
                trial["selectedHardware"] = writer.video_encoder_is_hardware();
                submit_frames(writer);
                trial["stage"] = "finalization";
                writer.finalize();
            }
            trial["succeeded"] = true;
            trial["stage"] = "complete";
            encoded = true;
        } catch (const winrt::hresult_error& error) {
            trial["hresult"] = error.code().value;
            trial["error"] = winrt::to_string(error.message());
        } catch (const std::exception& error) {
            trial["error"] = error.what();
        }
        report["encodingTrials"].push_back(std::move(trial));
    }
    std::ofstream output(directory / "probe.json", std::ios::binary);
    output << report.dump(2) << '\n';
    output.close();
    if (!output) throw std::runtime_error("探针报告写入失败");
    std::cout << report.dump(2) << '\n';
    return encoded ? 0 : 1;
}
} // namespace dogsc::cli
