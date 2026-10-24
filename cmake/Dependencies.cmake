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

if(WIN32)
    # 原生 ProRes 路径仅链接动态 LGPL FFmpeg 库，不启用默认功能集。
    if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "static")
        message(FATAL_ERROR "DogSC requires a dynamic FFmpeg vcpkg triplet (for example x64-windows).")
    endif()
    find_package(FFMPEG REQUIRED)
    if(NOT FFMPEG_INCLUDE_DIRS OR NOT FFMPEG_LIBRARIES)
        message(FATAL_ERROR "FFmpeg must provide avcodec, avformat, swscale and swresample through CMake.")
    endif()
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        file(GLOB _dogsc_ffmpeg_dlls
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/avcodec*.dll"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/avformat*.dll"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/avutil*.dll"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/swscale*.dll"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/swresample*.dll")
        list(LENGTH _dogsc_ffmpeg_dlls _dogsc_ffmpeg_dll_count)
        if(_dogsc_ffmpeg_dll_count LESS 5)
            message(FATAL_ERROR "The selected vcpkg triplet must install dynamic FFmpeg DLLs.")
        endif()
    endif()
endif()
