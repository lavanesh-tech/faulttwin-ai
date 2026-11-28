# Self-observability (Phase 5): an in-process metrics registry with Prometheus text
# exposition. Lock-free counters, gauges and fixed-bucket histograms (ADR-0019).
add_library(faulttwin_observability metrics.cpp)
add_library(faulttwin::observability ALIAS faulttwin_observability)
target_include_directories(faulttwin_observability PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_observability PUBLIC cxx_std_20)
target_link_libraries(faulttwin_observability PUBLIC faulttwin::core)
faulttwin_set_project_warnings(faulttwin_observability)
