include(FetchContent)

# GoogleTest is pinned to the commit behind the v1.17.0 release tag. Pinning a
# commit SHA (not a moving tag or branch) makes the test dependency reproducible.
# License: BSD-3-Clause (https://github.com/google/googletest/blob/main/LICENSE).
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723 # v1.17.0
  GIT_SHALLOW FALSE)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)
