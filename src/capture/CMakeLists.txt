add_library(dogsc_capture STATIC
    audio_capture.cpp
    media_writer.cpp
    ffmpeg_prores.cpp
    media_remux.cpp
    segmented_writer.cpp
    pointer_capture.cpp
    screen_capture.cpp
    camera_capture.cpp
)
target_include_directories(dogsc_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_directories(dogsc_capture PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_compile_definitions(dogsc_capture PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(dogsc_capture PUBLIC dogsc_core d3d11 dxgi windowsapp
    mf mfplat mfreadwrite mfuuid wmcodecdspuuid ole32 uuid dwmapi ${FFMPEG_LIBRARIES})
