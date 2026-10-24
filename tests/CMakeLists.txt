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
