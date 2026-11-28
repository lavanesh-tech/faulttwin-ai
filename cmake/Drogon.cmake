# Drogon HTTP framework for faulttwind (Phase 5, ADR-0021).
#
# FAULTTWIN_WITH_DROGON:
#   AUTO (default) - fetch and build Drogon if its required system library (jsoncpp) is found;
#                    otherwise skip faulttwind with a message. Everything else still builds.
#   ON             - Drogon is required.
#   OFF            - never build the HTTP server.
#
# Drogon v1.9.9 (MIT) and its event library Trantor (BSD-3-Clause, git submodule) are pinned
# to the commit behind the v1.9.9 tag. Only the HTTP core is built: no ORM, no drogon_ctl, no
# examples, no Brotli, no YAML config. Measured: about 1 minute on 4 cores (phase-05 report).
set(FAULTTWIN_WITH_DROGON "AUTO" CACHE STRING "Build faulttwind with Drogon: AUTO | ON | OFF")
set_property(CACHE FAULTTWIN_WITH_DROGON PROPERTY STRINGS AUTO ON OFF)

set(FAULTTWIN_HAVE_DROGON OFF)
if(NOT FAULTTWIN_WITH_DROGON STREQUAL "OFF")
  if(APPLE)
    find_program(FAULTTWIN_BREW_EXE brew)
    if(FAULTTWIN_BREW_EXE)
      execute_process(COMMAND "${FAULTTWIN_BREW_EXE}" --prefix jsoncpp
                      OUTPUT_VARIABLE faulttwin_jsoncpp_prefix
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE brew_rc)
      if(brew_rc EQUAL 0 AND IS_DIRECTORY "${faulttwin_jsoncpp_prefix}")
        list(APPEND CMAKE_PREFIX_PATH "${faulttwin_jsoncpp_prefix}")
      endif()
    endif()
  endif()
  find_path(FAULTTWIN_JSONCPP_INCLUDE json/json.h PATH_SUFFIXES jsoncpp)
  if(FAULTTWIN_JSONCPP_INCLUDE)
    set(FAULTTWIN_HAVE_DROGON ON)
  elseif(FAULTTWIN_WITH_DROGON STREQUAL "ON")
    message(FATAL_ERROR "FAULTTWIN_WITH_DROGON=ON but jsoncpp was not found "
                        "(apt install libjsoncpp-dev, or brew install jsoncpp)")
  else()
    message(STATUS "FaultTwin: jsoncpp not found; faulttwind (HTTP server) is skipped")
  endif()
endif()

if(FAULTTWIN_HAVE_DROGON)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_CTL OFF CACHE BOOL "" FORCE)
  set(BUILD_ORM OFF CACHE BOOL "" FORCE)
  set(BUILD_BROTLI OFF CACHE BOOL "" FORCE)
  set(BUILD_YAML_CONFIG OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  # No TLS in Trantor: faulttwind serves plain HTTP on loopback (TLS is Phase 11 hardening).
  # With the default ("" = use OpenSSL if found) the build depends on whichever OpenSSL the
  # machine has, and Trantor v1.5.x does not compile against OpenSSL 4.x (Homebrew on macOS).
  set(TRANTOR_USE_TLS "none" CACHE STRING "" FORCE)
  # Drogon's own headers and sources are third-party code: no -Werror, no clang-tidy.
  set(faulttwin_saved_tidy "${CMAKE_CXX_CLANG_TIDY}")
  set(CMAKE_CXX_CLANG_TIDY "")
  FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG 38dd5fea31a7a2727c0a6f6b6b04252374796cab # v1.9.9
    GIT_SUBMODULES trantor
    GIT_SHALLOW FALSE
    # SYSTEM: Drogon's and Trantor's headers are included as system headers, so our strict
    # warnings (-Wold-style-cast, -Wsign-conversion, ...) apply to our code only.
    SYSTEM)
  FetchContent_MakeAvailable(drogon)
  set(CMAKE_CXX_CLANG_TIDY "${faulttwin_saved_tidy}")
  message(STATUS "FaultTwin: Drogon v1.9.9 enabled; building faulttwind")
endif()
