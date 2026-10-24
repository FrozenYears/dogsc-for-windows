add_library(dogsc_render_d2d STATIC
    d2d_frame_compositor.cpp
)
target_include_directories(dogsc_render_d2d PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(dogsc_render_d2d PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(dogsc_render_d2d PUBLIC dogsc_core d3d11 dxgi d2d1 dwrite dxguid)
