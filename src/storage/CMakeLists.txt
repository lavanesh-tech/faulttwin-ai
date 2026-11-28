# Incident storage (Phase 5): the incident model, the IncidentStore interface, a bounded
# in-memory store and the recorder that turns detections into store operations.
# faulttwin_storage_pg adds the PostgreSQL meta-db store; it needs libpq (ADR-0020).
add_library(faulttwin_storage incident.cpp memory_incident_store.cpp incident_recorder.cpp)
add_library(faulttwin::storage ALIAS faulttwin_storage)
target_include_directories(faulttwin_storage PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(faulttwin_storage PUBLIC cxx_std_20)
target_link_libraries(faulttwin_storage PUBLIC faulttwin::detection faulttwin::observability)
faulttwin_set_project_warnings(faulttwin_storage)

# The SQL builds on faulttwin::collectors' SqlSession interface; the real session comes from
# faulttwin::collectors_pg, linked by whoever creates the store (the daemon, the tests).
add_library(faulttwin_storage_pg pg/pg_incident_store.cpp)
add_library(faulttwin::storage_pg ALIAS faulttwin_storage_pg)
target_link_libraries(faulttwin_storage_pg PUBLIC faulttwin::storage)
faulttwin_set_project_warnings(faulttwin_storage_pg)
