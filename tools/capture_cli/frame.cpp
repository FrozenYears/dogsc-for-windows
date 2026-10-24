#include "commands.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iterator>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <winrt/base.h>

#include "dogsc/project_json.hpp"

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
    if (!project_path.has_parent_path()) throw std::runtime_error("工程路径缺少父目录");
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

std::vector<std::uint8_t> read_frame_bmp(const std::filesystem::path& media,
                                         std::int64_t time_100ns, std::uint32_t max_width,
                                         std::uint32_t& output_width, std::uint32_t& output_height) {
    constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    winrt::com_ptr<IMFAttributes> attributes;
    winrt::check_hresult(MFCreateAttributes(attributes.put(), 1));
    winrt::check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
    winrt::com_ptr<IMFSourceReader> reader;
    winrt::check_hresult(MFCreateSourceReaderFromURL(media.c_str(), attributes.get(), reader.put()));
    winrt::com_ptr<IMFMediaType> requested;
    winrt::check_hresult(MFCreateMediaType(requested.put()));
    winrt::check_hresult(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
    winrt::check_hresult(reader->SetCurrentMediaType(stream, nullptr, requested.get()));

    winrt::com_ptr<IMFMediaType> actual;
    winrt::check_hresult(reader->GetCurrentMediaType(stream, actual.put()));
    UINT source_width = 0, source_height = 0;
    winrt::check_hresult(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &source_width, &source_height));
    if (source_width == 0 || source_height == 0) throw std::runtime_error("媒体帧尺寸无效");

    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = time_100ns;
    winrt::check_hresult(reader->SetCurrentPosition(GUID_NULL, position));
    PropVariantClear(&position);

    DWORD flags = 0;
    LONGLONG timestamp = 0;
    winrt::com_ptr<IMFSample> sample;
    while (true) {
        sample = nullptr;
        winrt::check_hresult(reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, sample.put()));
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) throw std::runtime_error("媒体没有可读取的帧");
        if (sample) break;
    }
    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
    BYTE* data = nullptr;
    DWORD max_length = 0, current_length = 0;
    winrt::check_hresult(buffer->Lock(&data, &max_length, &current_length));
    const auto unlock = [&] { buffer->Unlock(); };
    const auto source_stride = static_cast<std::size_t>(source_width) * 4;
    if (current_length < source_stride * source_height) { unlock(); throw std::runtime_error("媒体帧缓冲区过短"); }
    output_width = std::min<std::uint32_t>(source_width, std::max<std::uint32_t>(max_width, 1));
    output_height = std::max<std::uint32_t>(1, static_cast<std::uint64_t>(source_height) * output_width / source_width);
    const std::size_t pixel_bytes = static_cast<std::size_t>(output_width) * output_height * 4;
    std::vector<std::uint8_t> bmp(54 + pixel_bytes, 0);
    bmp[0] = 'B'; bmp[1] = 'M';
    put_u32(bmp, 2, static_cast<std::uint32_t>(bmp.size()));
    put_u32(bmp, 10, 54); put_u32(bmp, 14, 40);
    put_u32(bmp, 18, output_width); put_u32(bmp, 22, output_height);
    put_u16(bmp, 26, 1); put_u16(bmp, 28, 32); put_u32(bmp, 34, static_cast<std::uint32_t>(pixel_bytes));
    for (std::uint32_t y = 0; y < output_height; ++y) {
        const auto source_y = static_cast<std::uint64_t>(y) * source_height / output_height;
        auto* destination = bmp.data() + 54 + static_cast<std::size_t>(output_height - 1 - y) * output_width * 4;
        for (std::uint32_t x = 0; x < output_width; ++x) {
            const auto source_x = static_cast<std::uint64_t>(x) * source_width / output_width;
            const auto* source = data + source_y * source_stride + source_x * 4;
            std::memcpy(destination + static_cast<std::size_t>(x) * 4, source, 4);
        }
    }
    unlock();
    return bmp;
}
}  // namespace

int project_frame_command(const std::filesystem::path& project_path, double seconds,
                          std::uint32_t max_width) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("预览时间必须是非负有限数字");
    const auto normalized = project_path.extension() == ".json" ? project_path : project_path / "project.json";
    const auto media = media_path(normalized);
    std::uint32_t width = 0, height = 0;
    const auto bmp = read_frame_bmp(media, static_cast<std::int64_t>(seconds * 10000000), max_width, width, height);
    json result{{"time", seconds}, {"width", width}, {"height", height},
                {"mime", "image/bmp"}, {"data", "data:image/bmp;base64," + base64(bmp)}};
    std::cout << result.dump() << '\n';
    return 0;
}
}  // namespace dogsc::cli
