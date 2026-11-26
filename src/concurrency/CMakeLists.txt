# Bounded queue and reference unbounded queue. The queues are templates in
# include/faulttwin/concurrency/; this library holds the non-template parts.
add_library(faulttwin_concurrency queue_types.cpp)
add_library(faulttwin::concurrency ALIAS faulttwin_concurrency)

target_include_directories(faulttwin_concurrency PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_concurrency PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(faulttwin_concurrency PUBLIC Threads::Threads)
faulttwin_set_project_warnings(faulttwin_concurrency)
