#include "commands.hpp"
#include "dxgi_preview_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iterator>
#include <d3d11.h>
#include <d3d10.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <winrt/base.h>

#include "dogsc/project_json.hpp"
#include "dogsc/d2d_frame_compositor.hpp"
#include "dogsc/frame_scene_evaluator.hpp"
#include "dogsc/native_capture.hpp"

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

std::vector<std::uint8_t> png_from_bgra(IWICImagingFactory* factory,
                                        const std::vector<std::uint8_t>& pixels,
                                        std::uint32_t width, std::uint32_t height) {
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    const std::size_t pixel_size = stride * height;
    if (width == 0 || height == 0 || pixels.size() != pixel_size) {
        throw std::runtime_error("预览 BGRA 缓冲区无效");
    }
    if (!factory) throw std::invalid_argument("WIC PNG factory missing");
    winrt::com_ptr<IStream> output;
    winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, output.put()));
    winrt::com_ptr<IWICBitmapEncoder> encoder;
    winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    winrt::check_hresult(encoder->Initialize(output.get(), WICBitmapEncoderNoCache));
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
    winrt::check_hresult(frame->Initialize(nullptr));
    winrt::check_hresult(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    winrt::check_hresult(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) {
        throw std::runtime_error("WIC PNG 编码器不支持 BGRA 预览帧");
    }
    winrt::check_hresult(frame->WritePixels(height, static_cast<UINT>(stride),
        static_cast<UINT>(pixel_size), const_cast<BYTE*>(pixels.data())));
    winrt::check_hresult(frame->Commit());
    winrt::check_hresult(encoder->Commit());

    STATSTG information{};
    winrt::check_hresult(output->Stat(&information, STATFLAG_NONAME));
    const auto encoded_size = static_cast<std::size_t>(information.cbSize.QuadPart);
    std::vector<std::uint8_t> encoded(encoded_size);
    HGLOBAL memory = nullptr;
    winrt::check_hresult(GetHGlobalFromStream(output.get(), &memory));
    const auto* data = static_cast<const std::uint8_t*>(GlobalLock(memory));
    if (!data) throw std::runtime_error("无法读取 WIC PNG 预览帧");
    std::memcpy(encoded.data(), data, encoded_size);
    GlobalUnlock(memory);
    return encoded;
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
        winrt::check_hresult(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));

        D3D_FEATURE_LEVEL feature_level{};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT d3d_result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            nullptr, 0, D3D11_SDK_VERSION, d3d_device_.put(), &feature_level, d3d_context_.put());
        if (SUCCEEDED(d3d_result)) {
            winrt::com_ptr<ID3D10Multithread> multithread;
            d3d_result = d3d_context_->QueryInterface(__uuidof(ID3D10Multithread), multithread.put_void());
            if (SUCCEEDED(d3d_result)) multithread->SetMultithreadProtected(TRUE);
        }
        if (SUCCEEDED(d3d_result)) {
            UINT reset_token = 0;
            d3d_result = MFCreateDXGIDeviceManager(&reset_token, d3d_manager_.put());
            if (SUCCEEDED(d3d_result)) d3d_result = d3d_manager_->ResetDevice(d3d_device_.get(), reset_token);
            if (SUCCEEDED(d3d_result)) {
                d3d_result = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, d3d_manager_.get());
            }
            if (FAILED(d3d_result)) {
                d3d_manager_ = nullptr;
                d3d_context_ = nullptr;
                d3d_device_ = nullptr;
            }
        }
        uses_nv12_ = d3d_manager_.get() != nullptr;
        if (uses_nv12_) winrt::check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE));

        const auto configure_reader = [&]() {
            reader_ = nullptr;
            winrt::check_hresult(MFCreateSourceReaderFromURL(media.c_str(), attributes.get(), reader_.put()));
            winrt::com_ptr<IMFMediaType> requested;
            winrt::check_hresult(MFCreateMediaType(requested.put()));
            winrt::check_hresult(requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            winrt::check_hresult(requested->SetGUID(MF_MT_SUBTYPE,
                uses_nv12_ ? MFVideoFormat_NV12 : MFVideoFormat_RGB32));
            winrt::check_hresult(reader_->SetCurrentMediaType(stream, nullptr, requested.get()));
        };
        try {
            configure_reader();
        } catch (...) {
            if (!d3d_manager_) throw;
            reader_ = nullptr;
            (void)attributes->DeleteItem(MF_SOURCE_READER_D3D_MANAGER);
            d3d_manager_ = nullptr;
            d3d_context_ = nullptr;
            d3d_device_ = nullptr;
            uses_nv12_ = false;
            winrt::check_hresult(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
            configure_reader();
        }
        winrt::com_ptr<IMFMediaType> actual;
        winrt::check_hresult(reader_->GetCurrentMediaType(stream, actual.put()));
        winrt::check_hresult(MFGetAttributeSize(actual.get(), MF_MT_FRAME_SIZE, &width_, &height_));
        if (width_ == 0 || height_ == 0) throw std::runtime_error("媒体帧尺寸无效");
        if (uses_nv12_) {
            UINT32 raw_stride = 0;
            nv12_stride_ = SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))
                ? static_cast<std::uint32_t>(std::abs(static_cast<std::int32_t>(raw_stride))) : width_;
            UINT32 matrix = 0;
            const bool has_matrix = SUCCEEDED(actual->GetUINT32(MF_MT_YUV_MATRIX, &matrix));
            bt709_matrix_ = has_matrix ? matrix == MFVideoTransferMatrix_BT709 : width_ >= 1280;
            bt2020_matrix_ = has_matrix &&
                (matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12);
            UINT32 range = 0;
            limited_range_ = !SUCCEEDED(actual->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))
                || range == MFNominalRange_Unknown || range == MFNominalRange_16_235 || range == MFNominalRange_48_208;
        }
    }

    std::vector<std::uint8_t> read_bgra(std::int64_t time_100ns, std::uint32_t max_width,
                                       std::uint32_t& output_width, std::uint32_t& output_height,
                                       bool sequential = false) {
        constexpr DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        constexpr std::int64_t maximum_sequential_gap = 5'000'000;
        const bool continue_reading = sequential && has_last_sample_ && time_100ns > last_timestamp_ &&
            time_100ns - last_timestamp_ <= maximum_sequential_gap;
        if (!continue_reading) {
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = time_100ns;
            const HRESULT seek_result = reader_->SetCurrentPosition(GUID_NULL, position);
            PropVariantClear(&position);
            winrt::check_hresult(seek_result);
            has_last_sample_ = false;
        }
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        winrt::com_ptr<IMFSample> sample;
        while (true) {
            sample = nullptr;
            winrt::check_hresult(reader_->ReadSample(stream, 0, nullptr, &flags, &timestamp, sample.put()));
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) throw std::runtime_error("媒体没有可读取的帧");
            if (!sample) continue;
            last_timestamp_ = timestamp;
            has_last_sample_ = true;
            if (timestamp >= time_100ns) break;
        }
        output_width = std::min<std::uint32_t>(width_, std::max<std::uint32_t>(max_width, 1));
        output_height = std::max<std::uint32_t>(1, static_cast<std::uint64_t>(height_) * output_width / width_);
        const std::size_t pixel_bytes = static_cast<std::size_t>(output_width) * output_height * 4;
        std::vector<std::uint8_t> pixels(pixel_bytes, 0);

        winrt::com_ptr<IMFMediaBuffer> buffer;
        DWORD buffer_count = 0;
        winrt::check_hresult(sample->GetBufferCount(&buffer_count));
        if (buffer_count == 1) {
            winrt::check_hresult(sample->GetBufferByIndex(0, buffer.put()));
        } else {
            winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
        }
        BYTE* data = nullptr;
        std::size_t source_stride = uses_nv12_ ? nv12_stride_ : static_cast<std::size_t>(width_) * 4;
        DWORD current_length = 0;
        bool buffer_locked = false;
        bool texture_mapped = false;
        last_frame_used_dxgi_ = false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::com_ptr<IMFDXGIBuffer> dxgi_buffer;
        if (d3d_device_ && SUCCEEDED(buffer->QueryInterface(__uuidof(IMFDXGIBuffer), dxgi_buffer.put_void()))) {
            winrt::com_ptr<ID3D11Texture2D> source_texture;
            if (SUCCEEDED(dxgi_buffer->GetResource(__uuidof(ID3D11Texture2D), source_texture.put_void()))) {
                D3D11_TEXTURE2D_DESC source_desc{};
                source_texture->GetDesc(&source_desc);
                const bool bgra_format = source_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                    source_desc.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
                const bool supported_format = uses_nv12_ ? source_desc.Format == DXGI_FORMAT_NV12 : bgra_format;
                if (supported_format && source_desc.Width == width_ && source_desc.Height == height_ &&
                    source_desc.SampleDesc.Count == 1) {
                    if (!staging_ || staging_width_ != width_ || staging_height_ != height_ ||
                        staging_format_ != source_desc.Format) {
                        D3D11_TEXTURE2D_DESC staging_desc{};
                        staging_desc.Width = width_;
                        staging_desc.Height = height_;
                        staging_desc.MipLevels = 1;
                        staging_desc.ArraySize = 1;
                        staging_desc.Format = source_desc.Format;
                        staging_desc.SampleDesc.Count = 1;
                        staging_desc.Usage = D3D11_USAGE_STAGING;
                        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        winrt::check_hresult(d3d_device_->CreateTexture2D(&staging_desc, nullptr, staging_.put()));
                        staging_width_ = width_;
                        staging_height_ = height_;
                        staging_format_ = source_desc.Format;
                    }
                    UINT source_subresource = 0;
                    if (SUCCEEDED(dxgi_buffer->GetSubresourceIndex(&source_subresource))) {
                        d3d_context_->CopySubresourceRegion(staging_.get(), 0, 0, 0, 0,
                            source_texture.get(), source_subresource, nullptr);
                        if (SUCCEEDED(d3d_context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                            data = static_cast<BYTE*>(mapped.pData);
                            source_stride = mapped.RowPitch;
                            texture_mapped = true;
                            last_frame_used_dxgi_ = true;
                        }
                    }
                }
            }
        }
        if (!data) {
            BYTE* locked_data = nullptr;
            DWORD max_length = 0;
            winrt::check_hresult(buffer->Lock(&locked_data, &max_length, &current_length));
            buffer_locked = true;
            data = locked_data;
            const auto required_length = source_stride * height_ * (uses_nv12_ ? 3 : 1) / (uses_nv12_ ? 2 : 1);
            if (current_length < required_length) {
                buffer->Unlock();
                throw std::runtime_error("媒体帧缓冲区过短");
            }
        }
        for (std::uint32_t y = 0; y < output_height; ++y) {
            const auto source_y = static_cast<std::uint64_t>(y) * height_ / output_height;
            auto* destination = pixels.data() + static_cast<std::size_t>(y) * output_width * 4;
            for (std::uint32_t x = 0; x < output_width; ++x) {
                const auto source_x = static_cast<std::uint64_t>(x) * width_ / output_width;
                auto* pixel = destination + static_cast<std::size_t>(x) * 4;
                if (uses_nv12_) {
                    const auto y_value = static_cast<int>(data[source_y * source_stride + source_x]);
                    const auto chroma_index = source_stride * height_ + (source_y / 2) * source_stride + (source_x & ~std::uint64_t{1});
                    const int u = static_cast<int>(data[chroma_index]) - 128;
                    const int v = static_cast<int>(data[chroma_index + 1]) - 128;
                    const int y_scaled = limited_range_ ? std::max(y_value - 16, 0) : y_value;
                    int red = 0, green = 0, blue = 0;
                    if (bt2020_matrix_) {
                        red = limited_range_ ? (298 * y_scaled + 430 * v + 128) >> 8 : (256 * y_scaled + 377 * v + 128) >> 8;
                        green = limited_range_ ? (298 * y_scaled - 48 * u - 167 * v + 128) >> 8 : (256 * y_scaled - 42 * u - 146 * v + 128) >> 8;
                        blue = limited_range_ ? (298 * y_scaled + 548 * u + 128) >> 8 : (256 * y_scaled + 482 * u + 128) >> 8;
                    } else if (bt709_matrix_) {
                        red = limited_range_ ? (298 * y_scaled + 459 * v + 128) >> 8 : (256 * y_scaled + 403 * v + 128) >> 8;
                        green = limited_range_ ? (298 * y_scaled - 55 * u - 136 * v + 128) >> 8 : (256 * y_scaled - 48 * u - 120 * v + 128) >> 8;
                        blue = limited_range_ ? (298 * y_scaled + 541 * u + 128) >> 8 : (256 * y_scaled + 475 * u + 128) >> 8;
                    } else {
                        red = limited_range_ ? (298 * y_scaled + 409 * v + 128) >> 8 : (256 * y_scaled + 359 * v + 128) >> 8;
                        green = limited_range_ ? (298 * y_scaled - 100 * u - 208 * v + 128) >> 8 : (256 * y_scaled - 88 * u - 183 * v + 128) >> 8;
                        blue = limited_range_ ? (298 * y_scaled + 516 * u + 128) >> 8 : (256 * y_scaled + 454 * u + 128) >> 8;
                    }
                    pixel[0] = static_cast<std::uint8_t>(std::clamp(blue, 0, 255));
                    pixel[1] = static_cast<std::uint8_t>(std::clamp(green, 0, 255));
                    pixel[2] = static_cast<std::uint8_t>(std::clamp(red, 0, 255));
                } else {
                    const auto* source = data + source_y * source_stride + source_x * 4;
                    std::memcpy(pixel, source, 4);
                }
                pixel[3] = 0xff;
            }
        }
        if (texture_mapped) d3d_context_->Unmap(staging_.get(), 0);
        if (buffer_locked) buffer->Unlock();
        return pixels;
    }

    [[nodiscard]] std::int64_t last_timestamp() const { return last_timestamp_; }
    [[nodiscard]] bool used_dxgi_surface() const { return last_frame_used_dxgi_; }
    [[nodiscard]] bool has_dxgi_manager() const { return d3d_manager_.get() != nullptr; }

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    std::vector<std::uint8_t> encode_png(const std::vector<std::uint8_t>& pixels,
                                         std::uint32_t width, std::uint32_t height) {
        if (!wic_factory_) {
            winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IWICImagingFactory), wic_factory_.put_void()));
        }
        return png_from_bgra(wic_factory_.get(), pixels, width, height);
    }
private:
    winrt::com_ptr<IMFSourceReader> reader_;
    UINT width_ = 0;
    UINT height_ = 0;
    std::int64_t last_timestamp_ = 0;
    bool has_last_sample_ = false;
    winrt::com_ptr<ID3D11Device> d3d_device_;
    winrt::com_ptr<ID3D11DeviceContext> d3d_context_;
    winrt::com_ptr<IMFDXGIDeviceManager> d3d_manager_;
    winrt::com_ptr<ID3D11Texture2D> staging_;
    UINT staging_width_ = 0;
    UINT staging_height_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
    bool last_frame_used_dxgi_ = false;
    bool uses_nv12_ = false;
    std::uint32_t nv12_stride_ = 0;
    bool bt709_matrix_ = true;
    bool bt2020_matrix_ = false;
    bool limited_range_ = true;
    winrt::com_ptr<IWICImagingFactory> wic_factory_;
};

json frame_json(FrameReader& reader, double seconds, std::uint32_t max_width, bool sequential = false) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("预览时间必须是非负有限数字");
    std::uint32_t width = 0, height = 0;
    const auto pixels = reader.read_bgra(static_cast<std::int64_t>(seconds * 10000000), max_width,
        width, height, sequential);
    const auto png = reader.encode_png(pixels, width, height);
    return json{{"time", static_cast<double>(reader.last_timestamp()) / 10000000},
                {"width", width}, {"height", height},
                {"decodeSurface", reader.used_dxgi_surface() ? "DXGI" : "system-memory"},
                {"dxgiManager", reader.has_dxgi_manager()},
                {"mime", "image/png"}, {"data", "data:image/png;base64," + base64(png)}};
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


json composite_frame_json(const std::filesystem::path& project_path, double seconds,
                          std::uint32_t max_width) {
    const auto project = load_project(project_path);
    if (!project.media || !project.media->screen.source_end_time) throw std::runtime_error("工程缺少屏幕媒体时长");
    const auto duration = *project.media->screen.source_end_time - project.media->screen.source_start_time;
    if (!std::isfinite(duration) || duration <= 0) throw std::runtime_error("屏幕媒体时长无效");
    FrameReader reader(media_path(project_path));
    std::uint32_t width = 0, height = 0;
    const auto pixels = reader.read_bgra(static_cast<std::int64_t>(seconds * 10000000), max_width, width, height);
    const auto row_bytes = static_cast<std::size_t>(width) * 4;
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
    winrt::com_ptr<IWICImagingFactory> factory;
    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory), factory.put_void()));
    const auto png = png_from_bgra(factory.get(), output, width, height);
    return json{{"time", seconds}, {"width", width}, {"height", height},
                {"mime", "image/png"}, {"data", "data:image/png;base64," + base64(png)}};
}
winrt::com_ptr<ID3D11Texture2D> texture_from_bgra(ID3D11Device* device, ID3D11DeviceContext* context,
                                                   const std::vector<std::uint8_t>& pixels,
                                                   std::uint32_t width, std::uint32_t height) {
    const auto row_bytes = static_cast<std::size_t>(width) * 4;
    if (pixels.size() < row_bytes * height) throw std::runtime_error("媒体帧像素尺寸无效");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, texture.put()));
    context->UpdateSubresource(texture.get(), 0, nullptr, pixels.data(), static_cast<UINT>(row_bytes), 0);
    return texture;
}

json export_video_json(const std::filesystem::path& project_path, const std::filesystem::path& output,
                       int frame_rate) {
    const auto project = load_project(project_path);
    if (!project.media || !project.media->screen.source_end_time) throw std::runtime_error("工程缺少屏幕媒体时长");
    const auto duration = *project.media->screen.source_end_time - project.media->screen.source_start_time;
    if (!std::isfinite(duration) || duration <= 0) throw std::runtime_error("屏幕媒体时长无效");
    if (frame_rate < 1 || frame_rate > 120) throw std::invalid_argument("导出帧率必须在 1..120 之间");
    FrameReader reader(media_path(project_path));
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
    std::string error;
    auto compositor = D2DFrameCompositor::create(device.get(), error);
    if (!compositor) throw std::runtime_error("D2D 合成器创建失败: " + error);
    capture::WriterOptions options;
    options.path = output.wstring(); options.width = reader.width(); options.height = reader.height();
    options.frame_rate = frame_rate; options.codec = capture::VideoCodec::h264; options.fragmented = false;
    capture::MediaWriter writer(device.get(), options);
    const auto frame_count = static_cast<std::int64_t>(std::ceil(duration * frame_rate));
    const auto frame_duration = static_cast<std::int64_t>(10000000 / frame_rate);
    for (std::int64_t index = 0; index < frame_count; ++index) {
        const auto time = static_cast<double>(index) / frame_rate;
        std::uint32_t width = 0, height = 0;
        const auto pixels = reader.read_bgra(static_cast<std::int64_t>(time * 10000000),
            std::numeric_limits<std::uint32_t>::max(), width, height);
        const auto texture = texture_from_bgra(device.get(), context.get(), pixels, width, height);
        const auto plan = FrameSceneEvaluator::render_plan(project, time, duration, frame_rate,
            CompositionSize{static_cast<double>(width), static_cast<double>(height)});
        FrameRenderResources resources; resources.screen = texture.get();
        if (!compositor->compose(plan, resources)) throw std::runtime_error(std::string(compositor->last_error()));
        writer.write_video(compositor->output_texture(), index * frame_duration, frame_duration);
    }
    writer.finalize();
    return json{{"output", output.string()}, {"frameRate", frame_rate},
                {"frameCount", frame_count}, {"duration", duration},
                {"encoder", writer.video_encoder_name()}};
}
}  // namespace

int project_frame_command(const std::filesystem::path& project_path, double seconds,
                          std::uint32_t max_width) {
    const auto normalized = project_path.extension() == ".json" ? project_path : project_path / "project.json";
    FrameReader reader(media_path(normalized));
    std::cout << frame_json(reader, seconds, max_width).dump() << '\n';
    return 0;
}

int project_frame_stream_command(const std::filesystem::path& project_path, std::uint32_t max_width,
                                 std::uintptr_t preview_owner) {
    const auto normalized = project_path.extension() == ".json" ? project_path : project_path / "project.json";
    FrameReader reader(media_path(normalized));
    std::unique_ptr<DxgiPreviewOverlay> overlay;
    if (preview_owner) {
        try { overlay = std::make_unique<DxgiPreviewOverlay>(preview_owner); }
        catch (const std::exception& error) {
            std::cerr << "DXGI overlay initialization failed; using WebView PNG fallback: " << error.what() << '\n';
        }
    }
    RecorderProject project;
    double duration = 0;
    std::unique_ptr<D2DFrameCompositor> compositor;
    if (overlay) {
        try {
            project = load_project(normalized);
            if (!project.media || !project.media->screen.source_end_time) {
                throw std::runtime_error("工程缺少屏幕媒体时长");
            }
            duration = *project.media->screen.source_end_time - project.media->screen.source_start_time;
            if (!std::isfinite(duration) || duration <= 0) throw std::runtime_error("屏幕媒体时长无效");
            std::string error;
            compositor = D2DFrameCompositor::create(overlay->device(), error);
            if (!compositor) throw std::runtime_error(error);
        } catch (const std::exception& error) {
            std::cerr << "D2D preview initialization failed; using source frames: " << error.what() << '\n';
            compositor.reset();
        }
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "Q") break;
        if (line == "H") {
            if (overlay) overlay->set_bounds(0, 0, 0, 0, false);
            continue;
        }
        if (line.rfind("R ", 0) == 0) {
            int x = 0, y = 0, width = 0, height = 0;
            std::istringstream input(line.substr(2));
            if (!(input >> x >> y >> width >> height)) {
                std::cout << json{{"error", "预览窗口边界命令必须是 R <x> <y> <width> <height>"}}.dump() << '\n' << std::flush;
                continue;
            }
            if (overlay) {
                try { overlay->set_bounds(x, y, width, height, true); }
                catch (const std::exception& error) {
                    std::cout << json{{"error", error.what()}}.dump() << '\n' << std::flush;
                }
            }
            continue;
        }
        if (line.rfind("F ", 0) != 0) {
            std::cout << json{{"error", "预览流命令必须是 F <seconds>、R <bounds>、H 或 Q"}}.dump() << '\n' << std::flush;
            continue;
        }
        try {
            const auto seconds = std::stod(line.substr(2));
            if (!overlay) {
                std::cout << frame_json(reader, seconds, max_width, true).dump() << '\n' << std::flush;
                continue;
            }
            std::uint32_t width = 0, height = 0;
            auto pixels = reader.read_bgra(static_cast<std::int64_t>(seconds * 10000000),
                max_width, width, height, true);
            bool presented = false;
            bool composed = false;
            if (compositor) {
                try {
                    const auto texture = texture_from_bgra(overlay->device(), overlay->context(),
                        pixels, width, height);
                    const auto plan = FrameSceneEvaluator::render_plan(project, seconds, duration,
                        project.export_settings.frame_rate,
                        CompositionSize{static_cast<double>(width), static_cast<double>(height)});
                    FrameRenderResources resources;
                    resources.screen = texture.get();
                    if (!compositor->compose(plan, resources)) {
                        throw std::runtime_error(std::string(compositor->last_error()));
                    }
                    composed = true;
                } catch (const std::exception& error) {
                    std::cerr << "D2D preview composition failed; using source frame: " << error.what() << '\n';
                    compositor.reset();
                }
            }
            try {
                presented = composed
                    ? overlay->present_texture(compositor->output_texture())
                    : overlay->present(pixels, width, height);
            }
            catch (const std::exception& error) {
                std::cerr << "DXGI overlay presentation failed; using WebView PNG fallback: " << error.what() << '\n';
                overlay.reset();
                const auto png = reader.encode_png(pixels, width, height);
                std::cout << json{{"time", static_cast<double>(reader.last_timestamp()) / 10000000},
                    {"width", width}, {"height", height}, {"decodeSurface", reader.used_dxgi_surface() ? "DXGI" : "system-memory"},
                    {"dxgiManager", reader.has_dxgi_manager()}, {"presentation", "image"},
                    {"mime", "image/png"}, {"data", "data:image/png;base64," + base64(png)}}.dump() << '\n' << std::flush;
                continue;
            }
            auto response = json{{"time", static_cast<double>(reader.last_timestamp()) / 10000000},
                {"width", width}, {"height", height}, {"decodeSurface", reader.used_dxgi_surface() ? "DXGI" : "system-memory"},
                {"dxgiManager", reader.has_dxgi_manager()}, {"presentation", "dxgi"}, {"presented", presented},
                {"composed", composed}, {"presentedFrames", overlay->presented_frames()},
                {"droppedFrames", overlay->dropped_frames()}};
            if (const auto statistics = overlay->frame_statistics()) {
                response["frameStatistics"] = json{{"presentCount", statistics->present_count},
                    {"presentRefreshCount", statistics->present_refresh_count},
                    {"syncRefreshCount", statistics->sync_refresh_count},
                    {"syncQpcTime", statistics->sync_qpc_time}};
            }
            std::cout << response.dump() << '\n' << std::flush;
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
int project_export_video_command(const std::filesystem::path& project_path,
                                 const std::filesystem::path& output, int frame_rate) {
    const auto normalized = project_path.extension() == ".json" ? project_path : project_path / "project.json";
    std::cout << export_video_json(normalized, output, frame_rate).dump(2) << '\n';
    return 0;
}
}  // namespace dogsc::cli
