#include "dogsc/frame_compositor.hpp"

// M0 阶段只提供契约与图层顺序校验；具体后端在 M2a / M2b 实现。

namespace dogsc {
namespace {

// 编译期守卫：任意两个图层角色不得重复，避免后端重排时静默丢失图层。
constexpr bool no_duplicate_layers(const std::vector<FrameLayerRole>& order) {
    for (size_t i = 0; i < order.size(); ++i) {
        for (size_t j = i + 1; j < order.size(); ++j) {
            if (order[i] == order[j]) return false;
        }
    }
    return true;
}

}  // namespace

/// 校验图层顺序：必须是文档定义顺序的子序列。
///
/// 任何实现都不得自行发明顺序（例如把 cursor 画在 screen 之下），
/// 因此把这条规则放在共享层而不是各个后端里。
bool is_valid_layer_order(const std::vector<FrameLayerRole>& order) {
    static constexpr FrameLayerRole kCanonical[] = {
        FrameLayerRole::kBackground, FrameLayerRole::kScreen, FrameLayerRole::kCursor,
        FrameLayerRole::kSpotlight,  FrameLayerRole::kCamera, FrameLayerRole::kStickers,
    };
    if (!no_duplicate_layers(order)) return false;
    size_t next = 0;
    for (FrameLayerRole role : order) {
        while (next < std::size(kCanonical) && kCanonical[next] != role) ++next;
        if (next >= std::size(kCanonical)) return false;
        ++next;
    }
    return true;
}

}  // namespace dogsc
