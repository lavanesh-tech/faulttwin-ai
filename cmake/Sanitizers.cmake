# Sanitizer configurations. ASan+UBSan and TSan are mutually exclusive and are
# therefore separate build trees (see CMakePresets.json). Sanitizer builds are
# for correctness only; never use them for performance measurements.
function(faulttwin_configure_sanitizers)
  if(FAULTTWIN_SANITIZER STREQUAL "none")
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Sanitizers are only configured for GCC and Clang")
  endif()

  if(FAULTTWIN_SANITIZER STREQUAL "address-undefined")
    set(flags -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  elseif(FAULTTWIN_SANITIZER STREQUAL "thread")
    set(flags -fsanitize=thread -fno-omit-frame-pointer)
  else()
    message(FATAL_ERROR "Unknown FAULTTWIN_SANITIZER value: '${FAULTTWIN_SANITIZER}'")
  endif()

  message(STATUS "FaultTwin sanitizer build: ${FAULTTWIN_SANITIZER}")
  add_compile_options(${flags})
  add_link_options(${flags})
endfunction()
