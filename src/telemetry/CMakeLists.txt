add_library(faulttwin_telemetry
  node_id.cpp
  node_role.cpp
  validation.cpp
  synthetic.cpp)
add_library(faulttwin::telemetry ALIAS faulttwin_telemetry)

target_include_directories(faulttwin_telemetry PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_telemetry PUBLIC cxx_std_20)
faulttwin_set_project_warnings(faulttwin_telemetry)
