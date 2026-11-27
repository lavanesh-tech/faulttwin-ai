# faulttwin-detect: tunes the rule baseline on validation scenarios and evaluates it once on
# held-out test scenarios (Phase 4). Options and reports live in a library for unit tests.
add_library(faulttwin_detect_lib detect_options.cpp detect_report.cpp)
target_include_directories(faulttwin_detect_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faulttwin_detect_lib PUBLIC faulttwin::detection)
faulttwin_set_project_warnings(faulttwin_detect_lib)

add_executable(faulttwin-detect main.cpp)
target_link_libraries(faulttwin-detect PRIVATE faulttwin_detect_lib)
faulttwin_set_project_warnings(faulttwin-detect)
