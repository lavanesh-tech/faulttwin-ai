# The HTTP API as plain C++ (Phase 5): routing, validation, auth, JSON, error format,
# request ids, access logs and request metrics. No HTTP library here; the Drogon adapter in
# src/server translates requests (ADR-0021), so every route is unit-tested without a socket.
add_library(faulttwin_api http.cpp api_service.cpp)
add_library(faulttwin::api ALIAS faulttwin_api)
target_include_directories(faulttwin_api PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_api PUBLIC cxx_std_20)
target_link_libraries(faulttwin_api PUBLIC faulttwin::storage)
faulttwin_set_project_warnings(faulttwin_api)
