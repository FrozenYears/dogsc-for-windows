#pragma once

#include <memory>
#include <string>

#include <d3d11.h>

#include "dogsc/frame_compositor.hpp"

namespace dogsc {

/// 单线程离屏合成器。所有公共调用须由同一渲染线程串行执行。
class D2DFrameCompositor final : public IFrameCompositor {
public:
    /// WARP 与硬件后端使用相同渲染路径；失败原因写入 error。
    [[nodiscard]] static std::unique_ptr<D2DFrameCompositor> create_warp(std::string& error);
    [[nodiscard]] static std::unique_ptr<D2DFrameCompositor> create(
        ID3D11Device* device, std::string& error);
    ~D2DFrameCompositor() override;

    D2DFrameCompositor(const D2DFrameCompositor&) = delete;
    D2DFrameCompositor& operator=(const D2DFrameCompositor&) = delete;

    [[nodiscard]] std::string_view last_error() const override;
    /// 资源只在本次调用内借用；返回后不保留输入纹理引用。
    bool compose(const FrameRenderPlan& plan, const FrameRenderResources& resources) override;
    [[nodiscard]] CompositionSize output_size() const override;
    bool read_back(std::vector<uint8_t>& out) override;

    /// 借用指针；输入纹理须在此设备上创建，跨设备资源请由调用方显式传输。
    [[nodiscard]] ID3D11Device* device() const;
    /// 最近成功输出，premultiplied BGRA8/sRGB；下一次成功 compose 后失效。
    [[nodiscard]] ID3D11Texture2D* output_texture() const;

private:
    struct Impl;
    explicit D2DFrameCompositor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace dogsc
