# Rule-based anomaly detection (Phase 4): rules with sustain, hysteresis, cooldown,
# multi-signal conditions and severity; synthetic validation/test scenarios; scoring and
# tuning on validation scenarios only; k-NN and averaged-perceptron comparison detectors.
add_library(faulttwin_detection
  rule.cpp
  rule_engine.cpp
  scenario.cpp
  evaluation.cpp
  report.cpp
  online_detector.cpp
  learned.cpp)
add_library(faulttwin::detection ALIAS faulttwin_detection)
target_include_directories(faulttwin_detection PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_detection PUBLIC cxx_std_20)
target_link_libraries(faulttwin_detection PUBLIC faulttwin::features)
faulttwin_set_project_warnings(faulttwin_detection)
target_compile_options(faulttwin_detection PRIVATE -ffp-contract=off)
