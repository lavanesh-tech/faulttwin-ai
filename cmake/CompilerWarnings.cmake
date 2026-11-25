# Strict warnings for first-party targets only. Third-party code (GoogleTest)
# is deliberately excluded so that upstream warnings never break our build.
function(faulttwin_set_project_warnings target)
  set(common_warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wconversion
      -Wsign-conversion
      -Wold-style-cast
      -Wcast-align
      -Wnon-virtual-dtor
      -Woverloaded-virtual
      -Wnull-dereference
      -Wdouble-promotion
      -Wformat=2
      -Wimplicit-fallthrough
      -Wunused)

  set(gcc_only_warnings -Wduplicated-cond -Wduplicated-branches -Wlogical-op)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(warnings ${common_warnings} ${gcc_only_warnings})
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(warnings ${common_warnings})
  else()
    message(WARNING "No warning set defined for compiler ${CMAKE_CXX_COMPILER_ID}")
    set(warnings "")
  endif()

  if(FAULTTWIN_WARNINGS_AS_ERRORS)
    list(APPEND warnings -Werror)
  endif()

  target_compile_options(${target} PRIVATE ${warnings})

  if(FAULTTWIN_ENABLE_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${FAULTTWIN_CLANG_TIDY_EXE}")
  endif()
endfunction()
