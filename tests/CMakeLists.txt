add_executable(dogsc_core_tests
    core_tests.cpp
    test_framework.hpp
)

target_link_libraries(dogsc_core_tests PRIVATE dogsc_core)
target_compile_features(dogsc_core_tests PRIVATE cxx_std_20)

add_test(NAME core_tests COMMAND dogsc_core_tests)

add_executable(dogsc_capture_clock_tests capture_clock_tests.cpp)
target_include_directories(dogsc_capture_clock_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/capture)
target_link_libraries(dogsc_capture_clock_tests PRIVATE dogsc_core)
add_test(NAME capture_clock_tests COMMAND dogsc_capture_clock_tests)

add_executable(dogsc_scene_geometry_tests scene_geometry_tests.cpp)
target_link_libraries(dogsc_scene_geometry_tests PRIVATE dogsc_core)
add_test(NAME scene_geometry_tests COMMAND dogsc_scene_geometry_tests)

add_executable(dogsc_project_timeline_editing_tests project_timeline_editing_tests.cpp)
target_link_libraries(dogsc_project_timeline_editing_tests PRIVATE dogsc_core)
add_test(NAME project_timeline_editing_tests COMMAND dogsc_project_timeline_editing_tests)

add_executable(dogsc_frame_scene_tests frame_scene_tests.cpp)
target_link_libraries(dogsc_frame_scene_tests PRIVATE dogsc_core)
target_compile_features(dogsc_frame_scene_tests PRIVATE cxx_std_20)
add_test(NAME frame_scene_tests COMMAND dogsc_frame_scene_tests)

if(WIN32)
    add_executable(dogsc_d2d_smoke_tests d2d_smoke_tests.cpp)
    target_link_libraries(dogsc_d2d_smoke_tests PRIVATE dogsc_render_d2d)
    add_test(NAME d2d_warp_smoke COMMAND dogsc_d2d_smoke_tests)
endif()
