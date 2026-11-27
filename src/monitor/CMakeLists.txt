# faulttwin-monitor: PostgreSQL + host telemetry collector CLI (requires libpq).
add_library(faulttwin_monitor_lib monitor_options.cpp monitor_report.cpp)
target_include_directories(faulttwin_monitor_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faulttwin_monitor_lib PUBLIC faulttwin::collectors_pg faulttwin::detection)
faulttwin_set_project_warnings(faulttwin_monitor_lib)

add_executable(faulttwin-monitor main.cpp)
target_link_libraries(faulttwin-monitor PRIVATE faulttwin_monitor_lib)
faulttwin_set_project_warnings(faulttwin-monitor)
