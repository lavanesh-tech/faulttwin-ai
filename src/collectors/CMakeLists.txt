# PostgreSQL and host-resource collection.
#
# faulttwin_collectors holds everything that does not need libpq: the reading/quality model,
# LSN arithmetic, result parsing behind the SqlSession interface, rates, topology and the
# procfs resource collector. It is always built and fully unit-tested with fakes.
#
# faulttwin_collectors_pg is the thin libpq adapter. It is built only when libpq is found
# (FAULTTWIN_WITH_LIBPQ=AUTO, the default), so machines without libpq still build and test.
add_library(faulttwin_collectors
  reading.cpp
  lsn.cpp
  sql.cpp
  endpoint.cpp
  pg_collector.cpp
  rates.cpp
  topology.cpp
  linux_resources.cpp)
add_library(faulttwin::collectors ALIAS faulttwin_collectors)
target_include_directories(faulttwin_collectors PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_collectors PUBLIC cxx_std_20)
target_link_libraries(faulttwin_collectors PUBLIC faulttwin::telemetry)
faulttwin_set_project_warnings(faulttwin_collectors)

if(FAULTTWIN_HAVE_LIBPQ)
  add_library(faulttwin_collectors_pg pg/libpq_session.cpp)
  add_library(faulttwin::collectors_pg ALIAS faulttwin_collectors_pg)
  target_link_libraries(faulttwin_collectors_pg PUBLIC faulttwin::collectors
                                                PRIVATE PostgreSQL::PostgreSQL)
  faulttwin_set_project_warnings(faulttwin_collectors_pg)
endif()
