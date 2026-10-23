#pragma once

#include <optional>
#include <string>

#include "dogsc/project_model.hpp"

namespace dogsc {

enum class ProjectJsonErrorCode {
    kParseFailure,
    kNotAnObject,
    kMissingRequiredField,
    kInvalidFieldValue,
    kSchemaRejected,
};

/// 便捷入口：失败时返回空，不携带诊断信息。
/// 需要定位具体字段时使用带 `ProjectJsonError` 的重载。
[[nodiscard]] std::optional<RecorderProject> decode_project(const std::string& text);

struct ProjectJsonError {
    ProjectJsonErrorCode code = ProjectJsonErrorCode::kParseFailure;
    /// JSON Pointer 风格路径，便于定位到具体字段。
    std::string path;
    std::string detail;

    constexpr bool operator==(const ProjectJsonError&) const = default;
};

/// `project.json` 的唯一编解码入口。
///
/// 契约（见 `docs/15-final-plan.md` §3.1）：
/// 1. 所有字段「缺省即默认」，缺失不报错；
/// 2. 未知字段一律忽略，不写入、不报错；
/// 3. 写入时固定输出 schema 当前版本号；
/// 4. 读 → 写 → 读必须得到逐字段相等的工程。
class ProjectJson {
public:
    /// 解析工程文件。
    /// 返回值：成功时返回 true 并填充 `out`；失败时填充 `error`。
    static bool decode(const std::string& text, RecorderProject& out, ProjectJsonError& error);

    /// 序列化工程文件。缩进 2 空格，键顺序固定，便于人工 diff 与快照比对。
    static std::string encode(const RecorderProject& project);
};

}  // namespace dogsc
