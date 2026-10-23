#include <cstdio>

#include "dogsc/timeline_map.hpp"

namespace dogsc {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<Uuid> Uuid::parse(const std::string& text) {
    // 期望形态：8-4-4-4-12（36 字符）；允许带花括号。
    std::string digits;
    digits.reserve(32);
    for (char c : text) {
        if (c == '-' || c == '{' || c == '}') continue;
        const int value = hex_value(c);
        if (value < 0) return std::nullopt;
        digits.push_back(c);
    }
    if (digits.size() != 32) return std::nullopt;

    Uuid result;
    for (size_t i = 0; i < 16; ++i) {
        const int high = hex_value(digits[i * 2]);
        const int low = hex_value(digits[i * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result.bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return result;
}

std::string Uuid::to_string() const {
    // 小写、带连字符，与 Foundation `UUID.uuidString` 的解析结果对齐（仅大小写不同）。
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

bool Uuid::is_nil() const {
    for (uint8_t byte : bytes) {
        if (byte != 0) return false;
    }
    return true;
}

}  // namespace dogsc
