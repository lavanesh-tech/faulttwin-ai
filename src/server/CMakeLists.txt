# faulttwind (Phase 5): the daemon. faulttwin_server_lib holds everything except the HTTP
# framework (options, telemetry sources, monitor loop), so it is unit-tested without Drogon.
# It needs libpq (collectors and the meta-db store); the executable also needs Drogon.
add_library(faulttwin_server_lib daemon_options.cpp monitor_loop.cpp)
target_include_directories(faulttwin_server_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(faulttwin_server_lib PUBLIC faulttwin::api faulttwin::storage_pg
                                                  faulttwin::collectors_pg)
faulttwin_set_project_warnings(faulttwin_server_lib)

if(FAULTTWIN_HAVE_DROGON)
  add_executable(faulttwind main.cpp drogon_adapter.cpp)
  target_link_libraries(faulttwind PRIVATE faulttwin_server_lib drogon)
  target_compile_definitions(faulttwind PRIVATE FAULTTWIN_VERSION="${PROJECT_VERSION}")
  faulttwin_set_project_warnings(faulttwind)
endif()
