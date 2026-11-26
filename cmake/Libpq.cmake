# Locates libpq (the PostgreSQL C client library) for the collectors.
#
# FAULTTWIN_WITH_LIBPQ:
#   AUTO (default) - build the libpq adapter, faulttwin-monitor and integration tests if libpq
#                    is found; otherwise skip them with a message.
#   ON             - libpq is required.
#   OFF            - never use libpq.
#
# On macOS, Homebrew installs libpq "keg-only" (not linked into /opt/homebrew), so its prefix
# is added to the search path automatically when brew is available.
set(FAULTTWIN_WITH_LIBPQ "AUTO" CACHE STRING "Use libpq: AUTO | ON | OFF")
set_property(CACHE FAULTTWIN_WITH_LIBPQ PROPERTY STRINGS AUTO ON OFF)

set(FAULTTWIN_HAVE_LIBPQ OFF)
if(NOT FAULTTWIN_WITH_LIBPQ STREQUAL "OFF")
  if(APPLE)
    find_program(FAULTTWIN_BREW_EXE brew)
    if(FAULTTWIN_BREW_EXE)
      execute_process(COMMAND "${FAULTTWIN_BREW_EXE}" --prefix libpq
                      OUTPUT_VARIABLE faulttwin_libpq_prefix
                      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE brew_rc)
      if(brew_rc EQUAL 0 AND IS_DIRECTORY "${faulttwin_libpq_prefix}")
        list(APPEND CMAKE_PREFIX_PATH "${faulttwin_libpq_prefix}")
      endif()
    endif()
  endif()

  if(FAULTTWIN_WITH_LIBPQ STREQUAL "ON")
    find_package(PostgreSQL REQUIRED)
  else()
    find_package(PostgreSQL QUIET)
  endif()

  if(PostgreSQL_FOUND)
    set(FAULTTWIN_HAVE_LIBPQ ON)
    message(STATUS "FaultTwin: libpq ${PostgreSQL_VERSION_STRING} found; building faulttwin-monitor")
  else()
    message(STATUS "FaultTwin: libpq not found; faulttwin-monitor and integration tests are skipped")
  endif()
endif()
