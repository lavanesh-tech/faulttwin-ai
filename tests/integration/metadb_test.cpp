// Integration tests for PgIncidentStore against the lab meta-db (compose.yaml service meta-db).
//
// Run only when FT_INTEGRATION=1 (scripts/run-integration-tests.sh loads .env and sets it).
// Each test starts from empty tables. The same contract cases as MemoryIncidentStore run here.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "../unit/storage/incident_store_contract.hpp"
#include "faulttwin/collectors/libpq_session.hpp"
#include "faulttwin/storage/pg_incident_store.hpp"

namespace faulttwin::storage {
namespace {

std::optional<std::string> env(const char* name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): tests read the environment from one thread
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
}

bool integration_enabled() {
    return env("FT_INTEGRATION") == "1";
}

collectors::PgEndpoint metadb_endpoint() {
    const auto port = env("FT_METADB_PORT").value_or("55435");
    return {"meta-db", env("FT_METADB_HOST").value_or("127.0.0.1"),
            static_cast<std::uint16_t>(std::stoul(port))};
}

collectors::PgCredentials metadb_credentials() {
    return {env("FT_METADB_USER").value_or("ft_app"), env("FT_METADB_PASSWORD").value_or(""),
            env("FT_METADB_DBNAME").value_or("faulttwin")};
}

std::variant<std::unique_ptr<collectors::LibpqSession>, collectors::SqlError> connect_metadb() {
    collectors::ConnectOptions options;
    options.read_only = false;
    options.application_name = "faulttwin-integration-tests";
    options.query_timeout = std::chrono::milliseconds{5000};
    options.statement_timeout = std::chrono::milliseconds{4000};
    return collectors::LibpqSession::connect(metadb_endpoint(), metadb_credentials(), options);
}

SessionFactory session_factory() {
    return []() -> std::variant<std::unique_ptr<collectors::SqlSession>, collectors::SqlError> {
        auto session = connect_metadb();
        if (auto* error = std::get_if<collectors::SqlError>(&session)) {
            return *error;
        }
        return std::unique_ptr<collectors::SqlSession>(
            std::move(std::get<std::unique_ptr<collectors::LibpqSession>>(session)));
    };
}

/// Runs one statement as ft_app; fails the test on error.
collectors::SqlTable exec(const std::string& sql) {
    auto session = connect_metadb();
    if (auto* error = std::get_if<collectors::SqlError>(&session)) {
        ADD_FAILURE() << "connect: " << error->message;
        return {};
    }
    auto result = std::get<std::unique_ptr<collectors::LibpqSession>>(session)->query(sql);
    if (auto* error = std::get_if<collectors::SqlError>(&result)) {
        ADD_FAILURE() << sql << ": " << error->message;
        return {};
    }
    return std::get<collectors::SqlTable>(result);
}

/// Returns the SQL error for a statement that is expected to fail, or nullopt if it succeeded.
std::optional<std::string> exec_error(const std::string& sql) {
    auto session = connect_metadb();
    if (auto* error = std::get_if<collectors::SqlError>(&session)) {
        return "connect: " + error->message;
    }
    auto result = std::get<std::unique_ptr<collectors::LibpqSession>>(session)->query(sql);
    if (auto* error = std::get_if<collectors::SqlError>(&result)) {
        return error->message;
    }
    return std::nullopt;
}

std::unique_ptr<IncidentStore> fresh_pg_store() {
    if (!integration_enabled()) {
        return nullptr;
    }
    auto store = std::make_unique<PgIncidentStore>(session_factory());
    auto migrated = store->migrate();
    if (auto* error = std::get_if<StoreError>(&migrated)) {
        ADD_FAILURE() << "migrate: " << error->message;
        return nullptr;
    }
    (void)exec("TRUNCATE faulttwin.incident_events, faulttwin.incidents RESTART IDENTITY");
    return store;
}

#define REQUIRE_METADB()                                                         \
    do {                                                                         \
        if (!integration_enabled()) {                                            \
            GTEST_SKIP() << "set FT_INTEGRATION=1 with the lab meta-db running " \
                            "(scripts/run-integration-tests.sh)";                \
        }                                                                        \
    } while (false)

TEST(MetaDb, MigrateIsIdempotent) {
    REQUIRE_METADB();
    PgIncidentStore store(session_factory());
    auto first = store.migrate();
    ASSERT_TRUE(std::holds_alternative<int>(first)) << std::get<StoreError>(first).message;
    auto second = store.migrate();
    ASSERT_TRUE(std::holds_alternative<int>(second)) << std::get<StoreError>(second).message;
    EXPECT_EQ(std::get<int>(second), kMetaDbSchemaVersion);
    const auto rows = exec("SELECT count(*) FROM faulttwin.schema_migrations");
    ASSERT_EQ(rows.rows.size(), 1U);
    EXPECT_EQ(rows.rows[0][0].value_or(""), std::to_string(kMetaDbSchemaVersion));
}

TEST(MetaDb, RefusesASchemaNewerThanThisBuild) {
    REQUIRE_METADB();
    PgIncidentStore store(session_factory());
    ASSERT_TRUE(std::holds_alternative<int>(store.migrate()));
    const auto future = std::to_string(kMetaDbSchemaVersion + 1);
    (void)exec("INSERT INTO faulttwin.schema_migrations (version) VALUES (" + future + ")");
    auto refused = store.migrate();
    (void)exec("DELETE FROM faulttwin.schema_migrations WHERE version = " + future);
    ASSERT_TRUE(std::holds_alternative<StoreError>(refused));
    EXPECT_EQ(std::get<StoreError>(refused).kind, StoreErrorKind::kInvalid);
}

TEST(MetaDb, OneOpenIncidentPerRuleAndNodeIsEnforcedByTheDatabase) {
    REQUIRE_METADB();
    auto store = fresh_pg_store();
    ASSERT_NE(store, nullptr);
    ASSERT_TRUE(
        std::holds_alternative<std::uint64_t>(store->open(contract::new_incident("r", "a"))));
    auto duplicate = store->open(contract::new_incident("r", "a"));
    ASSERT_TRUE(std::holds_alternative<StoreError>(duplicate));
    EXPECT_EQ(std::get<StoreError>(duplicate).kind, StoreErrorKind::kConflict);
}

TEST(MetaDb, AppRoleHasNoPrivilegesOutsideItsDatabase) {
    REQUIRE_METADB();
    const auto rows = exec(
        "SELECT rolsuper, rolcreaterole, rolcreatedb, rolreplication, rolbypassrls "
        "FROM pg_roles WHERE rolname = current_user");
    ASSERT_EQ(rows.rows.size(), 1U);
    for (const auto& cell : rows.rows[0]) {
        EXPECT_EQ(cell.value_or(""), "f");
    }
    EXPECT_TRUE(exec_error("CREATE DATABASE faulttwin_should_not_exist").has_value());
    EXPECT_TRUE(exec_error("CREATE ROLE faulttwin_should_not_exist").has_value());
    EXPECT_TRUE(exec_error("CREATE TABLE public.faulttwin_should_not_exist (x int)").has_value());
}

TEST(MetaDb, ConcurrentWritersSerialiseOnOneStore) {
    REQUIRE_METADB();
    auto store = fresh_pg_store();
    ASSERT_NE(store, nullptr);
    std::vector<std::jthread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&store, w] {
            for (int i = 0; i < 10; ++i) {
                auto r = store->open(
                    contract::new_incident("r" + std::to_string(i), "node-" + std::to_string(w)));
                EXPECT_TRUE(std::holds_alternative<std::uint64_t>(r));
            }
        });
    }
    writers.clear();
    auto stats = store->stats();
    ASSERT_TRUE(std::holds_alternative<StoreStats>(stats));
    EXPECT_EQ(std::get<StoreStats>(stats).open, 40U);
}

}  // namespace

namespace contract {
INSTANTIATE_TEST_SUITE_P(Postgres, IncidentStoreContract,
                         ::testing::Values(StoreFactory{fresh_pg_store}));
}  // namespace contract

}  // namespace faulttwin::storage
