#include "dogsc/camera_capture.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace dogsc::capture {
namespace {

void camera_check(HRESULT result, const wchar_t* message) {
    if (FAILED(result)) throw winrt::hresult_error(result, message);
}

struct CameraFormat {
    UINT32 width = 0;
    UINT32 height = 0;
    LONG stride = 0;
};

CameraFormat camera_format(IMFSourceReader* reader) {
    winrt::com_ptr<IMFMediaType> type;
    winrt::check_hresult(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        type.put()));
    GUID subtype{};
    winrt::check_hresult(type->GetGUID(MF_MT_SUBTYPE, &subtype));
    if (subtype != MFVideoFormat_RGB32) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
    CameraFormat format;
    winrt::check_hresult(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE,
        &format.width, &format.height));
    if (!format.width || !format.height ||
        format.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        format.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
    }
    UINT32 stride = 0;
    const auto result = type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride);
    if (result == MF_E_ATTRIBUTENOTFOUND) {
        winrt::check_hresult(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
            format.width, &format.stride));
    } else {
        winrt::check_hresult(result);
        format.stride = static_cast<LONG>(stride);
    }
    return format;
}

struct PendingCameraFrame {
    winrt::com_ptr<IMFSample> sample;
    std::int64_t host_time = 0;
    LONG stride = 0;
    bool device_qpc = false;
};

struct CameraState {
    // 请求锁只串行化 ReadSample/Flush，等待回调时不得持有它或状态锁。
    std::mutex request_mutex;
    std::mutex mutex;
    std::condition_variable flushed_condition;
    winrt::com_ptr<IMFSourceReader> reader;
    CameraFormat format;
    PendingCameraFrame latest;
    HRESULT error = S_OK;
    bool running = false;
    bool flushed = false;

    void fail(HRESULT result) {
        std::lock_guard lock(mutex);
        if (running && SUCCEEDED(error)) error = result;
    }

    void request_next() {
        std::lock_guard request_lock(request_mutex);
        winrt::com_ptr<IMFSourceReader> current;
        {
            std::lock_guard lock(mutex);
            if (!running || FAILED(error)) return;
            current = reader;
        }
        const auto result = current->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
            0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(result)) fail(result);
    }
};

struct CameraReaderCallback : winrt::implements<CameraReaderCallback, IMFSourceReaderCallback> {
    explicit CameraReaderCallback(const std::shared_ptr<CameraState>& state) : state_(state) {}

    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status, DWORD, DWORD flags,
        LONGLONG, IMFSample* sample) noexcept override {
        const auto state = state_.lock();
        if (!state) return S_OK;
        try {
            // 在任何格式检查/锁等待之前记录主机到达时间；它不等于曝光时间。
            const auto arrival = host_time_100ns();
            camera_check(status, L"摄像头读取失败；请检查设备连接和 Windows 摄像头隐私权限");
            if (flags & MF_SOURCE_READERF_ERROR) winrt::throw_hresult(E_FAIL);
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) winrt::throw_hresult(MF_E_END_OF_STREAM);
            if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                         MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
                winrt::com_ptr<IMFSourceReader> reader;
                {
                    std::lock_guard lock(state->mutex);
                    if (!state->running) return S_OK;
                    reader = state->reader;
                }
                const auto format = camera_format(reader.get());
                std::lock_guard lock(state->mutex);
                if (format.width != state->format.width || format.height != state->format.height) {
                    winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
                }
                state->format.stride = format.stride;
            }
            if (sample) {
                PendingCameraFrame frame;
                frame.sample.copy_from(sample);
                frame.host_time = arrival;
                UINT64 timestamp = 0;
                // 官方定义 DeviceTimestamp 为 QPC 同原点的 100ns，不使用未保证 QPC 的
                // DeviceReferenceSystemTime，也不把相对流时间戳猜成主机时间。
                // https://learn.microsoft.com/windows/win32/medfound/mfsampleextension-devicetimestamp
                const auto result = sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &timestamp);
                if (result == MF_E_ATTRIBUTENOTFOUND) {
                    frame.device_qpc = false;
                } else {
                    camera_check(result, L"读取摄像头设备 QPC 时间戳失败");
                    if (timestamp > static_cast<UINT64>((std::numeric_limits<std::int64_t>::max)())) {
                        winrt::throw_hresult(MF_E_INVALID_TIMESTAMP);
                    }
                    frame.host_time = static_cast<std::int64_t>(timestamp);
                    frame.device_qpc = true;
                }
                std::lock_guard lock(state->mutex);
                if (!state->running) return S_OK;
                frame.stride = state->format.stride;
                // 只有一个拥有引用的待取样本，丢弃积压旧帧。
                state->latest = std::move(frame);
            }
            state->request_next();
        } catch (...) {
            state->fail(winrt::to_hresult());
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnFlush(DWORD) noexcept override {
        if (const auto state = state_.lock()) {
            std::lock_guard lock(state->mutex);
            state->flushed = true;
            state->flushed_condition.notify_all();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEvent(DWORD, IMFMediaEvent* event) noexcept override {
        if (const auto state = state_.lock()) {
            HRESULT status = S_OK;
            const auto result = event ? event->GetStatus(&status) : E_POINTER;
            if (FAILED(result)) state->fail(result);
            else if (FAILED(status)) state->fail(status);
        }
        return S_OK;
    }

private:
    std::weak_ptr<CameraState> state_;
};

struct CaptureDeviceList {
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    ~CaptureDeviceList() {
        for (UINT32 index = 0; index < count; ++index) devices[index]->Release();
        CoTaskMemFree(devices);
    }
};

// 兼容有独立表面 pitch 的 2D 缓冲区及普通连续 RGB32 缓冲区。
struct CameraBufferLock {
    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::com_ptr<IMF2DBuffer2> two_dimensional;
    BYTE* top = nullptr;
    BYTE* begin = nullptr;
    DWORD length = 0;
    LONG pitch = 0;
    bool locked = false;

    CameraBufferLock(IMFSample* sample, LONG stride, UINT32 height) {
        winrt::check_hresult(sample->ConvertToContiguousBuffer(buffer.put()));
        two_dimensional = buffer.try_as<IMF2DBuffer2>();
        if (two_dimensional) {
            winrt::check_hresult(two_dimensional->Lock2DSize(MF2DBuffer_LockFlags_Read,
                &top, &pitch, &begin, &length));
            locked = true;
        } else {
            winrt::check_hresult(buffer->Lock(&begin, nullptr, &length));
            locked = true;
            pitch = stride;
            top = begin;
            if (pitch < 0) {
                const auto offset = static_cast<std::uint64_t>(-static_cast<std::int64_t>(pitch)) *
                    (height - 1);
                if (offset >= length) {
                    buffer->Unlock();
                    locked = false;
                    winrt::throw_hresult(MF_E_BUFFERTOOSMALL);
                }
                top += static_cast<std::size_t>(offset);
            }
        }
    }

    ~CameraBufferLock() {
        if (!locked) return;
        if (two_dimensional) two_dimensional->Unlock2D();
        else buffer->Unlock();
    }
};

} // namespace

struct CameraCapture::Impl {
    std::shared_ptr<CameraState> state = std::make_shared<CameraState>();
    winrt::com_ptr<IMFMediaSource> source;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> texture;
    std::vector<std::uint8_t> pixels;
    bool started = false;
    bool stopped = false;
    bool device_qpc = false;

    explicit Impl(ID3D11Device* supplied_device) {
        if (!supplied_device) throw std::invalid_argument("摄像头需要有效的 D3D11 设备");
        device.copy_from(supplied_device);
        device->GetImmediateContext(context.put());
        try {
            winrt::com_ptr<IMFAttributes> attributes;
            winrt::check_hresult(MFCreateAttributes(attributes.put(), 1));
            winrt::check_hresult(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
            CaptureDeviceList devices;
            camera_check(MFEnumDeviceSources(attributes.get(), &devices.devices, &devices.count),
                L"枚举摄像头失败；请检查 Windows 摄像头隐私权限");
            if (!devices.count) throw std::runtime_error("未找到 Media Foundation 摄像头设备");
            camera_check(devices.devices[0]->ActivateObject(__uuidof(IMFMediaSource),
                source.put_void()), L"打开首个摄像头失败；请检查设备占用和 Windows 摄像头隐私权限");
            auto callback = winrt::make_self<CameraReaderCallback>(state);
            winrt::com_ptr<IMFAttributes> reader_attributes;
            winrt::check_hresult(MFCreateAttributes(reader_attributes.put(), 2));
            winrt::check_hresult(reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK,
                callback.get()));
            winrt::check_hresult(reader_attributes->SetUINT32(
                MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
            camera_check(MFCreateSourceReaderFromMediaSource(source.get(), reader_attributes.get(),
                state->reader.put()), L"创建摄像头 SourceReader 失败");
            winrt::check_hresult(state->reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE));
            winrt::check_hresult(state->reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE));
            winrt::com_ptr<IMFMediaType> output;
            winrt::check_hresult(MFCreateMediaType(output.put()));
            winrt::check_hresult(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
            winrt::check_hresult(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
            camera_check(state->reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                nullptr, output.get()), L"摄像头不支持 SourceReader RGB32 解码/转换");
            state->format = camera_format(state->reader.get());
            const auto& format = state->format;
            pixels.resize(static_cast<std::size_t>(format.width) * format.height * 4);
            D3D11_TEXTURE2D_DESC description{};
            description.Width = format.width;
            description.Height = format.height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            winrt::check_hresult(device->CreateTexture2D(&description, nullptr, texture.put()));
        } catch (...) {
            state->reader = nullptr;
            if (source) source->Shutdown();
            throw;
        }
    }
};

CameraCapture::CameraCapture(ID3D11Device* device) : impl_(std::make_unique<Impl>(device)) {}
CameraCapture::~CameraCapture() {
    try { stop(); } catch (...) {}
}
std::uint32_t CameraCapture::width() const { return impl_->state->format.width; }
std::uint32_t CameraCapture::height() const { return impl_->state->format.height; }
bool CameraCapture::timestamp_is_device_qpc() const { return impl_->device_qpc; }

void CameraCapture::start() {
    auto& impl = *impl_;
    if (impl.stopped) throw std::logic_error("摄像头已停止，不能重新启动");
    {
        std::lock_guard lock(impl.state->mutex);
        camera_check(impl.state->error, L"摄像头采集失败；请检查连接、占用及隐私权限");
        if (impl.started) return;
        impl.state->running = true;
        impl.started = true;
    }
    impl.state->request_next();
    std::lock_guard lock(impl.state->mutex);
    camera_check(impl.state->error, L"启动摄像头读取失败；请检查 Windows 摄像头隐私权限");
}

ScreenFrame CameraCapture::poll() {
    auto& impl = *impl_;
    PendingCameraFrame frame;
    {
        std::lock_guard lock(impl.state->mutex);
        camera_check(impl.state->error, L"摄像头采集失败；请检查连接、占用及隐私权限");
        if (!impl.state->running) return {};
        frame = std::move(impl.state->latest);
        impl.state->latest = {};
    }
    if (!frame.sample) return {};
    const auto width = impl.state->format.width;
    const auto height = impl.state->format.height;
    const auto row_bytes = static_cast<std::size_t>(width) * 4;
    {
        CameraBufferLock buffer(frame.sample.get(), frame.stride, height);
        const auto begin = reinterpret_cast<std::uintptr_t>(buffer.begin);
        const auto top = reinterpret_cast<std::uintptr_t>(buffer.top);
        const auto absolute_pitch = buffer.pitch < 0 ? -static_cast<std::int64_t>(buffer.pitch) :
            static_cast<std::int64_t>(buffer.pitch);
        if (!buffer.begin || !buffer.top || absolute_pitch < static_cast<std::int64_t>(row_bytes) ||
            top < begin || top - begin >= buffer.length) {
            winrt::throw_hresult(MF_E_BUFFERTOOSMALL);
        }
        const auto initial_offset = static_cast<std::int64_t>(top - begin);
        for (UINT32 row = 0; row < height; ++row) {
            const auto offset = initial_offset + static_cast<std::int64_t>(row) * buffer.pitch;
            if (offset < 0 || static_cast<std::uint64_t>(offset) + row_bytes > buffer.length) {
                winrt::throw_hresult(MF_E_BUFFERTOOSMALL);
            }
            auto* destination = impl.pixels.data() + static_cast<std::size_t>(row) * row_bytes;
            std::memcpy(destination, buffer.begin + offset, row_bytes);
            // RGB32 的第四字节未定义；提供明确不透明的 BGRA 给合成器。
            for (UINT32 column = 0; column < width; ++column) destination[column * 4 + 3] = 255;
        }
    }
    // 只有调用者线程更新纹理；回调不使用 D3D context，不会覆盖同步编码中的帧。
    impl.context->UpdateSubresource(impl.texture.get(), 0, nullptr, impl.pixels.data(),
        static_cast<UINT>(row_bytes), 0);
    impl.device_qpc = frame.device_qpc;
    return {impl.texture, frame.host_time};
}

void CameraCapture::stop() {
    auto& impl = *impl_;
    if (impl.stopped) return;
    impl.stopped = true;
    HRESULT flush_result = S_OK;
    winrt::com_ptr<IMFSourceReader> reader;
    {
        std::lock_guard request_lock(impl.state->request_mutex);
        {
            std::lock_guard lock(impl.state->mutex);
            impl.state->running = false;
            impl.state->latest = {};
            impl.state->flushed = false;
            reader = impl.state->reader;
        }
        if (impl.started && reader) flush_result = reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS));
    }
    if (impl.started && reader && SUCCEEDED(flush_result)) {
        std::unique_lock lock(impl.state->mutex);
        // 设备失联时也不无限等待驱动；回调仅弱引用独立状态，没有悬空 Impl 指针。
        if (!impl.state->flushed_condition.wait_for(lock, std::chrono::seconds(5),
            [&] { return impl.state->flushed; })) {
            flush_result = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
    }
    const auto shutdown_result = impl.source ? impl.source->Shutdown() : S_OK;
    {
        std::lock_guard lock(impl.state->mutex);
        impl.state->reader = nullptr;
    }
    reader = nullptr;
    impl.source = nullptr;
    camera_check(flush_result, L"摄像头取消读取/等待 Flush 失败");
    camera_check(shutdown_result, L"摄像头关闭失败");
}

} // namespace dogsc::capture
