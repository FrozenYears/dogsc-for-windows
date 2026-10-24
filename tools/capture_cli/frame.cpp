#include "commands.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <winrt/base.h>

#include "dogsc/project_json.hpp"
#include "dogsc/d2d_frame_compositor.hpp"
#include "dogsc/frame_scene_evaluator.hpp"

namespace dogsc::cli {
namespace {
using json = nlohmann::ordered_json;

std::string base64(const std::vector<std::uint8_t>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto remaining = std::min<std::size_t>(3, bytes.size() - index);
        const std::uint32_t value = (static_cast<std::uint32_t>(bytes[index]) << 16) |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) << 8 : 0) |
            (remaining > 2 ? bytes[index + 2] : 0);
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(remaining > 1 ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(remaining > 2 ? alphabet[value & 63] : '=');
    }
    return result;
}

void put_u16(std::vector<std::uint8_t>& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::uint8_t>(value);
    output[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put_u32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) output[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

std::filesystem::path media_path(const std::filesystem::path& project_path) {
    RecorderProject project;
    ProjectJsonError error;
    std::ifstream input(project_path, std::ios::binary);
    if (!input) throw std::runtime_error("无法打开工程文件");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!ProjectJson::decode(text, project, error)) throw std::runtime_error("工程解析失败: " + error.detail);
    if (!project.media || project.media->screen.relative_path.empty()) {
        throw std::runtime_error("工程缺少屏幕媒体引用");
    }
    const std::filesystem::path relative(project.media->screen.relative_path);
    if (relative.is_absolute()) throw std::runtime_error("屏幕媒体路径不能是绝对路径");
    for (const auto& part : relative) if (part == "..") throw std::runtime_error("屏幕媒体路径越界");
    return project_path.parent_path() / relative;
}

class FrameReader {
public:
    explicit FrameReader(const std::filesystem::path& media) {
        constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        winrt::com_ptr<IMFAttributes> attributes;
        winrt::check_hresult(MFCreateAttributes(attributes.put(), 1));
        winrt::check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
        winrt::check_hresult(MFCreateSourceReaderFromURL(media.c_str(), attributes.get(), reader_.put()));
        winrt::com_ptr<IMFMediaType> requested;
        winrt::check_hresult(MFCreateMediaType(requested.put()));
        winrt::check_hresult(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        winrt::check_hresult(requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
        winrt::check_hresult(reader_->SetCurrentMediaType(stream, nullptr, requested.get()));
        winrt::com_ptr<IMFMediaType> actual;
        winrt::check_hresult(reader_->GetCurrentMediaType(stream, actual.put()));
        winrt::check_hresult(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &width_, &height_));
        if (width_ == 0 || height_ == 0) throw std::runtime_error("媒体帧尺寸无效");
    }

    std::vector<std::uint8_t> read_bmp(std::int64_t time_100ns, std::uint32_t max_width,
                                       std::uint32_t& output_width, std::uint32_t& output_height) {
        constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = time_100ns;
        winrt::check_hresult(reader_->SetCurrentPosition(GUID_NULL, position));
        PropVariantClear(&position);
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        winrt::com_ptr<IMFSample> sample;
        while (true) {
            sample = nullptr;
            winrt::check_hresult(reader_->ReadSample(stream, 0, nullptr, &flags, &timestamp, sample.put()));
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) throw std::runtime_error("媒体没有可读取的帧");
            if (sample) break;
        }
        winrt::com_ptr<IMFMediaBuffer> buffer;
        winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
        BYTE* data = nullptr;
        DWORD max_length = 0, current_length = 0;
        winrt::check_hresult(buffer->Lock(&data, &max_length, &current_length));
        const auto unlock = [&] { buffer->Unlock(); };
        const auto source_stride = static_cast<std::size_t>(width_) * 4;
        if (current_length < source_stride * height_) {
            unlock();
            throw std::runtime_error("媒体帧缓冲区过短");
        }
        output_width = std::min<std::uint32_t>(width_, std::max<std::uint32_t>(max_width, 1));
        output_height = std::max<std::uint32_t>(1, static_cast<std::uint64_t>(height_) * output_width / width_);
        const std::size_t pixel_bytes = static_cast<std::size_t>(output_width) * output_height * 4;
        std::vector<std::uint8_t> bmp(54 + pixel_bytes, 0);
        bmp[0] = 'B'; bmp[1] = 'M';
        put_u32(bmp, 2, static_cast<std::uint32_t>(bmp.size()));
        put_u32(bmp, 10, 54); put_u32(bmp, 14, 40);
        put_u32(bmp, 18, output_width); put_u32(bmp, 22, output_height);
        put_u16(bmp, 26, 1); put_u16(bmp, 28, 32); put_u32(bmp, 34, static_cast<std::uint32_t>(pixel_bytes));
        for (std::uint32_t y = 0; y < output_height; ++y) {
            const auto source_y = static_cast<std::uint64_t>(y) * height_ / output_height;
            auto* destination = bmp.data() + 54 + static_cast<std::size_t>(output_height - 1 - y) * output_width * 4;
            for (std::uint32_t x = 0; x < output_width; ++x) {
                const auto source_x = static_cast<std::uint64_t>(x) * width_ / output_width;
                const auto* source = data + source_y * source_stride + source_x * 4;
                std::memcpy(destination + static_cast<std::size_t>(x) * 4, source, 4);
            }
        }
        unlock();
        return bmp;
    }

private:
    winrt::com_ptr<IMFSourceReader> reader_;
    UINT width_ = 0;
    UINT height_ = 0;
};

json frame_json(FrameReader& reader, double seconds, std::uint32_t max_width) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("预览时间必须是非负有限数字");
    std::uint32_t width = 0, height = 0;
    const auto bmp = reader.read_bmp(static_cast<std::int64_t>(seconds * 10000000), max_width, width, height);
    return json{{"time", seconds}, {"width", width}, {"height", height},
                {"mime", "image/bmp"}, {"data", "data:image/bmp;base64," + base64(bmp)}};
}

RecorderProject load_project(const std::filesystem::path& path) {
    RecorderProject project;
    ProjectJsonError error;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法打开工程文件");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!ProjectJson::decode(text, project, error)) throw std::runtime_error("工程解析失败: " + error.detail);
    return project;
}

std::vector<std::uint8_t> bmp_from_bgra(const std::vector<std::uint8_t>& pixels,
                                        std::uint32_t width, std::uint32_t height) {
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    if (pixels.size() < row_bytes * height) throw std::runtime_error("合成输出尺寸无效");
    std::vector<std::uint8_t> bmp(54 + row_bytes * height, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    put_u32(bmp, 2, static_cast<std::uint32_t>(bmp.size()));
    put_u32(bmp, 10, 54); put_u32(bmp, 14, 40);
    put_u32(bmp, 18, width); put_u32(bmp, 22, height);
    put_u16(bmp, 26, 1); put_u16(bmp, 28, 32); put_u32(bmp, 34, static_cast<std::uint32_t>(row_bytes * height));
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* source = pixels.data() + static_cast<std::size_t>(y) * row_bytes;
        auto* destination = bmp.data() + 54 + static_cast<std::size_t>(height - 1 - y) * row_bytes;
        std::memcpy(destination, source, row_bytes);
    }
    return bmp;
}

json composite_frame_json(const std::filesystem::path& project_path, double seconds,
                          std::uint32_t max_width) {
    const auto project = load_project(project_path);
    if (!project.media || !project.media->screen.source_end_time) throw std::runtime_error("工程缺少屏幕媒体时长");
    const auto duration = *project.media->screen.source_end_time - project.media->screen.source_start_time;
    if (!std::isfinite(duration) || duration <= 0) throw std::runtime_error("屏幕媒体时长无效");
    FrameReader reader(media_path(project_path));
    std::uint32_t width = 0, height = 0;
    const auto input_bmp = reader.read_bmp(static_cast<std::int64_t>(seconds * 10000000), max_width, width, height);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    const auto row_bytes = static_cast<std::size_t>(width) * 4;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* source = input_bmp.data() + 54 + static_cast<std::size_t>(height - 1 - y) * row_bytes;
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * row_bytes, source, row_bytes);
    }
    std::string error;
    auto compositor = D2DFrameCompositor::create_warp(error);
    if (!compositor) throw std::runtime_error("D2D WARP 合成器创建失败: " + error);
    winrt::com_ptr<ID3D11DeviceContext> context;
    compositor->device()->GetImmediateContext(context.put());
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(compositor->device()->CreateTexture2D(&desc, nullptr, texture.put()));
    context->UpdateSubresource(texture.get(), 0, nullptr, pixels.data(), static_cast<UINT>(row_bytes), 0);
    const auto plan = FrameSceneEvaluator::render_plan(project, seconds, duration,
        project.export_settings.frame_rate, CompositionSize{static_cast<double>(width), static_cast<double>(height)});
    FrameRenderResources resources;
    resources.screen = texture.get();
    if (!compositor->compose(plan, resources)) throw std::runtime_error(std::string(compositor->last_error()));
    std::vector<std::uint8_t> output;
    if (!compositor->read_back(output)) throw std::runtime_error(std::string(compositor->last_error()));
    const auto bmp = bmp_from_bgra(output, width, height);
    return json{{"time", seconds}, {"width", width}, {"height", height},
                {"mime", "image/bmp"}, {"data", "data:image/bmp;base64," + base64(bmp)}};
}
}  // namespace

int project_frame_command(const std::filesystem::path& project_path, double seconds,
                          std::uint32_t max_width) {
    const auto normalized = project_path.extension() == ".json" ? project_path : project_path / "project.json";
    FrameReader reader(media_path(normalized));
    std::cout << frame_json(reader, seconds, max_width).dump() << '\n';
    return 0;
}

int project_frame_stream_command(const std::filesystem::path& project_path, std::uint32_t max_width) {
    const auto normalized = project_path.extension() == ".json" ? project_path : project_path / "project.json";
    FrameReader reader(media_path(normalized));
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "Q") break;
        if (line.rfind("F ", 0) != 0) {
            std::cout << json{{"error", "预览流命令必须是 F <seconds> 或 Q"}}.dump() << '\n' << std::flush;
            continue;
        }
        try {
            const auto seconds = std::stod(line.substr(2));
            std::cout << frame_json(reader, seconds, max_width).dump() << '\n' << std::flush;
        } catch (const std::exception& error) {
            std::cout << json{{"error", error.what()}}.dump() << '\n' << std::flush;
        }
    }
    return 0;
}
int project_composite_frame_command(const std::filesystem::path& project_path, double seconds,
                                    std::uint32_t max_width) {
    const auto normalized = project_path.extension() == ".json" ? project_path : project_path / "project.json";
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("预览时间必须是非负有限数字");
    std::cout << composite_frame_json(normalized, seconds, max_width).dump() << '\n';
    return 0;
}
}  // namespace dogsc::cli
