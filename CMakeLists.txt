cmake_minimum_required(VERSION 3.25)

project(dogsc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 浮点求值必须与 macOS 端逐位可比：禁用 fast-math。
if(MSVC)
    add_compile_options(/W4 /permissive- /fp:precise /Zc:__cplusplus /utf-8)
endif()

option(DOGSC_BUILD_TESTS "构建核心层单元测试" ON)

add_subdirectory(src/core)

add_subdirectory(tools/scene_dump)

if(DOGSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
