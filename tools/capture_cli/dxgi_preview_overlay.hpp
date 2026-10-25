#pragma once

#include <cstdint>
#include <memory>
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
    [[nodiscard]] std::uint64_t presented_frames() const;
    [[nodiscard]] std::uint64_t dropped_frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dogsc::cli
