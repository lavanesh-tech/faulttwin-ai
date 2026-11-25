# Option parsing and report formatting live in a small library so that they can
# be unit tested; main.cpp only wires them together.
add_library(faulttwin_cli_lib options.cpp report.cpp)
target_include_directories(faulttwin_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faulttwin_cli_lib PUBLIC faulttwin::telemetry)
faulttwin_set_project_warnings(faulttwin_cli_lib)

add_executable(faulttwin-synth main.cpp)
target_link_libraries(faulttwin-synth PRIVATE faulttwin_cli_lib)
faulttwin_set_project_warnings(faulttwin-synth)
