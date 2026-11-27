# Time-series features (Phase 4): event-time rolling windows with Welford statistics, the
# versioned feature schema, the extractor and adapters from collector output.
add_library(faulttwin_features
  rolling_window.cpp
  feature_schema.cpp
  feature_extractor.cpp
  adapters.cpp)
add_library(faulttwin::features ALIAS faulttwin_features)
target_include_directories(faulttwin_features PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_features PUBLIC cxx_std_20)
target_link_libraries(faulttwin_features PUBLIC faulttwin::collectors)
faulttwin_set_project_warnings(faulttwin_features)
# Deterministic arithmetic (ADR-0016): never fuse a*b+c into an FMA, which some targets
# (Apple Silicon) do by default and which changes results in the last bits.
target_compile_options(faulttwin_features PRIVATE -ffp-contract=off)
