cmake_minimum_required(VERSION 3.25)

project(
  FaultTwinAI
  VERSION 0.1.0
  DESCRIPTION "AI-powered distributed database failure prediction and recovery engine"
  LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported. Use a preset, e.g. `cmake --preset debug`.")
endif()

# ---- Language standard (applies to every target, first- and third-party) -----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Project options ----------------------------------------------------------
option(FAULTTWIN_BUILD_TESTS "Build FaultTwin unit and CLI tests" ON)
option(FAULTTWIN_WARNINGS_AS_ERRORS "Treat compiler warnings in first-party code as errors" ON)
option(FAULTTWIN_ENABLE_CLANG_TIDY "Run clang-tidy on first-party code while compiling" OFF)
option(FAULTTWIN_BUILD_BENCHMARKS "Build Google Benchmark benchmarks (use only with Release)" OFF)
set(FAULTTWIN_SANITIZER "none" CACHE STRING "Sanitizer build: none | address-undefined | thread")
set_property(CACHE FAULTTWIN_SANITIZER PROPERTY STRINGS none address-undefined thread)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Libpq)

# Sanitizer flags must be applied before third-party code is added so that the
# whole program (including GoogleTest) is instrumented consistently.
faulttwin_configure_sanitizers()

if(FAULTTWIN_ENABLE_CLANG_TIDY)
  find_program(FAULTTWIN_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

# ---- First-party targets ------------------------------------------------------
add_subdirectory(src)

# ---- Tests --------------------------------------------------------------------
if(FAULTTWIN_BUILD_TESTS)
  include(Dependencies)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- Benchmarks ---------------------------------------------------------------
if(FAULTTWIN_BUILD_BENCHMARKS)
  include(BenchmarkDependency)
  add_subdirectory(bench)
endif()
