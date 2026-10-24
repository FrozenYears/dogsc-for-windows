#include "dogsc/pointer_capture.hpp"

#include <algorithm>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <winrt/base.h>

namespace dogsc::capture {
namespace {
const char* cursor_asset(const CURSORINFO& cursor) {
    if (!(cursor.flags & CURSOR_SHOWING)) return "system.hidden";
    if (cursor.hCursor == LoadCursorW(nullptr, IDC_HAND)) return "system.pointing-hand";
    if (cursor.hCursor == LoadCursorW(nullptr, IDC_IBEAM)) return "system.ibeam";
    if (cursor.hCursor == LoadCursorW(nullptr, IDC_CROSS)) return "system.crosshair";
    if (cursor.hCursor == LoadCursorW(nullptr, IDC_NO)) return "system.not-allowed";
    return "system.arrow";
}
}
PointerCapture::PointerCapture() {
    if (current_) throw std::logic_error("同一线程已有指针采集器");
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, hook, GetModuleHandleW(nullptr), 0);
    winrt::check_bool(hook_ != nullptr);
    current_ = this;
}
PointerCapture::~PointerCapture() {
    if (hook_) UnhookWindowsHookEx(hook_);
    current_ = nullptr;
}
LRESULT CALLBACK PointerCapture::hook(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && current_) {
        const char* kind = nullptr;
        if (message == WM_MOUSEMOVE) kind = "move";
        if (message == WM_LBUTTONDOWN) kind = "leftClick";
        if (message == WM_RBUTTONDOWN) kind = "rightClick";
        if (kind) {
            // 回调不得抛出异常到 Win32；分配失败时设置线程退出请求。
            try {
                const auto* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
                CURSORINFO cursor{sizeof(CURSORINFO)};
                GetCursorInfo(&cursor);
                const auto delay = static_cast<std::uint32_t>(GetTickCount() - event->time);
                current_->events_.push_back({host_time_100ns() - static_cast<std::int64_t>(delay) * 10000,
                    event->pt, kind,
                    (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                    (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
                    (GetAsyncKeyState(VK_MENU) & 0x8000) != 0, cursor_asset(cursor)});
            } catch (...) {
                PostQuitMessage(ERROR_NOT_ENOUGH_MEMORY);
            }
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
void PointerCapture::drain(const RecordingClock& clock, RECT bounds, std::ostream& output) {
    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) throw std::invalid_argument("指针归一化区域无效");
    const auto write_event = [&](const Event& event) {
        const auto time = clock.map(event.host);
        if (!time) return;
        nlohmann::ordered_json modifiers = nlohmann::ordered_json::array();
        if (event.control) modifiers.push_back("control");
        if (event.shift) modifiers.push_back("shift");
        if (event.alt) modifiers.push_back("option");
        nlohmann::ordered_json value{
            {"time", static_cast<double>(*time) / 10000000.0},
            {"location", {{"x", std::clamp(static_cast<double>(event.point.x - bounds.left) / width, 0.0, 1.0)},
                          {"y", std::clamp(static_cast<double>(event.point.y - bounds.top) / height, 0.0, 1.0)}}},
            {"kind", event.kind}, {"modifiers", modifiers}, {"cursorAssetID", event.asset_id}};
        output << value.dump() << '\n';
    };
    while (!events_.empty()) {
        write_event(events_.front());
        events_.pop_front();
    }
    // 静止指针也留一个位置种子；低级钩子被 UIPI 限制时仍可记录位置。
    CURSORINFO cursor{sizeof(CURSORINFO)};
    if (GetCursorInfo(&cursor)) {
        write_event({host_time_100ns(), cursor.ptScreenPos, "move",
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0,
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0, cursor_asset(cursor)});
    }
    if (!output) throw std::runtime_error("指针轨写入失败");
}
} // namespace dogsc::capture
