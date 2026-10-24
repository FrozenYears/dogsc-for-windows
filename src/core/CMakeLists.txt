include(${CMAKE_SOURCE_DIR}/cmake/Dependencies.cmake)

add_library(dogsc_core STATIC
    basic_types.cpp
    cursor_asset.cpp
    frame_compositor.cpp
    scene_geometry.cpp
    frame_scene_evaluator.cpp
    frame_tracks.cpp
    pointer_track.cpp
    media_timeline.cpp
    output_frame_schedule.cpp
    project_model.cpp
    project_timeline.cpp
    timeline_map.cpp
    project_timeline_editing.cpp
    timeline_media_plan.cpp
    timing_guards.cpp
    project_json.cpp
    uuid.cpp
)

target_include_directories(dogsc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dogsc_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(dogsc_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(dogsc_core PRIVATE /W4 /permissive-)
endif()
