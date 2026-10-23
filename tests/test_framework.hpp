#pragma once

// 极简断言框架：不引入第三方测试库，减少 M0 的依赖面。
// 失败即打印文件行号并返回非零退出码。

#include <cmath>
#include <cstdio>
#include <string>

namespace dogsc::test {

inline int& failure_count() {
    static int count = 0;
    return count;
}

inline void report_failure(const char* file, int line, const char* expression) {
    std::fprintf(stderr, "%s:%d: 断言失败: %s\n", file, line, expression);
    ++failure_count();
}

inline void expect_near(double actual, double expected, double tolerance, const char* file,
                        int line, const char* expression) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        std::fprintf(stderr, "%s:%d: 断言失败: %s（实际 %.17g，期望 %.17g，容差 %g）\n", file,
                     line, expression, actual, expected, tolerance);
        ++failure_count();
    }
}

}  // namespace dogsc::test

#define EXPECT_TRUE(expr)                                                       \
    do {                                                                        \
        if (!(expr)) ::dogsc::test::report_failure(__FILE__, __LINE__, #expr);   \
    } while (false)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(actual, expected)                                             \
    do {                                                                        \
        if (!((actual) == (expected)))                                           \
            ::dogsc::test::report_failure(__FILE__, __LINE__, #actual " == " #expected); \
    } while (false)

#define EXPECT_NEAR(actual, expected, tolerance)                                \
    do {                                                                        \
        ::dogsc::test::expect_near((actual), (expected), (tolerance), __FILE__,  \
                                   __LINE__, #actual " ≈ " #expected);           \
    } while (false)
