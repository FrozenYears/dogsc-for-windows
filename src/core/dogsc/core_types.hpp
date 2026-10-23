#pragma once

#include <cstdint>
#include <string>

namespace dogsc {

/// 与 macOS 端 `TimeInterval` 对应的秒数类型。全部时间运算使用 double，
/// 禁止用 float 参与任何时间线计算。
using TimeInterval = double;

/// 90,000 时基，可被 60/90/120 整除。
inline constexpr int32_t kPresentationTimescale = 90000;

}  // namespace dogsc
