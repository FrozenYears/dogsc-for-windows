#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <d3d11.h>
#include <vector>

namespace dogsc::cli {

class DxgiPreviewOverlay {
public:
    explicit DxgiPreviewOverlay(std::uintptr_t owner_window);
    ~DxgiPreviewOverlay();
    DxgiPreviewOverlay(const DxgiPreviewOverlay&) = delete;
    DxgiPreviewOverlay& operator=(const DxgiPreviewOverlay&) = delete;

    void set_bounds(int x, int y, int width, int height, bool visible);
    bool present(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t height);
    struct FrameStatistics {
        std::uint32_t present_count = 0;
        std::uint32_t present_refresh_count = 0;
        std::uint32_t sync_refresh_count = 0;
        std::int64_t sync_qpc_time = 0;
    };
    [[nodiscard]] std::optional<FrameStatistics> frame_statistics() const;
    [[nodiscard]] ID3D11Device* device() const;
    [[nodiscard]] ID3D11DeviceContext* context() const;
    bool present_texture(ID3D11Texture2D* texture);
    [[nodiscard]] std::uint64_t presented_frames() const;
    [[nodiscard]] std::uint64_t dropped_frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dogsc::cli
