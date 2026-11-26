# Ingest pipeline: shared per-event processing, ordering/duplicate tracking,
# the concurrent producer/consumer pipeline and the single-threaded baseline.
add_library(faulttwin_ingest
  order_tracker.cpp
  event_processor.cpp
  pipeline.cpp
  baseline.cpp)
add_library(faulttwin::ingest ALIAS faulttwin_ingest)

target_include_directories(faulttwin_ingest PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_ingest PUBLIC cxx_std_20)
target_link_libraries(faulttwin_ingest PUBLIC faulttwin::telemetry faulttwin::concurrency)
faulttwin_set_project_warnings(faulttwin_ingest)
