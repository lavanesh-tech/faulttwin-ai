# Shared building blocks (Phase 5): a bounded strict JSON parser and writer helpers, UTC
# time formatting/parsing, and the structured logger. No third-party dependencies.
add_library(faulttwin_core json.cpp time.cpp logger.cpp)
add_library(faulttwin::core ALIAS faulttwin_core)
target_include_directories(faulttwin_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_core PUBLIC cxx_std_20)
faulttwin_set_project_warnings(faulttwin_core)
