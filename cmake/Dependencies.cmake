# 第三方依赖统一入口。
#
# M0 只依赖一个 JSON 库。vcpkg 可用时走 vcpkg 清单模式，否则用
# FetchContent 拉取同名版本，保证开发者机器不需要预装 vcpkg。
include_guard(GLOBAL)

find_package(nlohmann_json 3.11 QUIET)

if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()
