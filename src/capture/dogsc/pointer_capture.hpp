#pragma once

#include "dogsc/native_capture.hpp"
#include "dogsc/recording_clock.hpp"
#include <deque>
#include <ostream>

namespace dogsc::capture {

// 低级钩子只入队；调用者必须泵消息并定期 drain，不能在钩子里写盘。
class PointerCapture {
public:
    PointerCapture();
    ~PointerCapture();
    PointerCapture(const PointerCapture&) = delete;
    PointerCapture& operator=(const PointerCapture&) = delete;
    void drain(const RecordingClock& clock, RECT bounds, std::ostream& output);
private:
    struct Event {
        std::int64_t host;
        POINT point;
        const char* kind;
        bool control;
        bool shift;
        bool alt;
        const char* asset_id;
    };
    static LRESULT CALLBACK hook(int code, WPARAM message, LPARAM data);
    inline static thread_local PointerCapture* current_ = nullptr;
    HHOOK hook_ = nullptr;
    std::deque<Event> events_;
};
} // namespace dogsc::capture
