// faulttwind: monitor loop + incident recorder + HTTP API in one process (ADR-0002).
//
// Threads: main runs Drogon's event loop; Drogon owns --http-threads I/O threads that call
// ApiService::handle; the monitor loop owns one std::jthread. Shutdown on SIGINT/SIGTERM:
// readiness turns 503 ("draining"), Drogon stops accepting and returns from run(), then the
// monitor loop is stopped and joined and the recorder flushes what it still holds.

#include <drogon/drogon.h>
#include <pthread.h>

#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "daemon_options.hpp"
#include "drogon_adapter.hpp"
#include "faulttwin/api/api_service.hpp"
#include "faulttwin/collectors/libpq_session.hpp"
#include "faulttwin/detection/rule.hpp"
#include "faulttwin/features/feature_schema.hpp"
#include "faulttwin/storage/memory_incident_store.hpp"
#include "faulttwin/storage/pg_incident_store.hpp"
#include "monitor_loop.hpp"

namespace {

namespace srv = faulttwin::server;
namespace core = faulttwin::core;
namespace storage = faulttwin::storage;
namespace api = faulttwin::api;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitStartup = 3;

#ifndef FAULTTWIN_VERSION
#define FAULTTWIN_VERSION "unknown"
#endif

std::unique_ptr<storage::IncidentStore> make_store(const srv::DaemonOptions& options,
                                                   core::Logger& logger) {
    if (options.store == srv::StoreKind::kMemory) {
        logger.warn("store", "using the in-memory incident store; incidents are lost on exit");
        return std::make_unique<storage::MemoryIncidentStore>();
    }
    faulttwin::collectors::ConnectOptions connect;
    connect.read_only = false;
    connect.application_name = "faulttwind";
    connect.query_timeout = std::chrono::milliseconds{3000};
    connect.statement_timeout = std::chrono::milliseconds{2500};
    auto store = std::make_unique<storage::PgIncidentStore>(
        [endpoint = options.metadb.endpoint, credentials = options.metadb.credentials,
         connect]() -> std::variant<std::unique_ptr<faulttwin::collectors::SqlSession>,
                                    faulttwin::collectors::SqlError> {
            auto session =
                faulttwin::collectors::LibpqSession::connect(endpoint, credentials, connect);
            if (auto* error = std::get_if<faulttwin::collectors::SqlError>(&session)) {
                return *error;
            }
            return std::unique_ptr<faulttwin::collectors::SqlSession>(
                std::move(std::get<std::unique_ptr<faulttwin::collectors::LibpqSession>>(session)));
        });
    auto migrated = store->migrate();
    if (auto* error = std::get_if<storage::StoreError>(&migrated)) {
        // Startup refuses to run against a schema it cannot use; an unreachable meta-db is
        // also fatal at startup (but only degrades readiness once running).
        logger.error(
            "store", "meta-db migration failed",
            {{"error", std::string(storage::to_string(error->kind))}, {"detail", error->message}});
        return nullptr;
    }
    logger.info("store", "meta-db schema ready",
                {{"schema_version", std::get<int>(migrated)},
                 {"host", options.metadb.endpoint.host},
                 {"port", static_cast<std::uint64_t>(options.metadb.endpoint.port)}});
    return store;
}

sigset_t shutdown_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    return set;
}

// clang-tidy 18 reports every object below as "can be declared const", although each one is
// mutated through a reference (making them const does not compile). Known false positive.
// NOLINTBEGIN(misc-const-correctness)
int run(const srv::DaemonOptions& options) {
    core::Logger logger = core::Logger::to_stderr(options.log_level);
    faulttwin::observability::Registry metrics;
    auto store = make_store(options, logger);
    if (!store) {
        return kExitStartup;
    }

    const auto& schema = faulttwin::features::default_feature_schema();
    const auto rules = faulttwin::detection::default_rule_set();
    storage::IncidentRecorder recorder(*store, logger, schema.version(), 1024, &metrics);
    recorder.start(core::WallClock::now());

    api::StatusBoard board;
    api::ApiConfig config;
    config.api_key = options.api_key;
    config.build_version = FAULTTWIN_VERSION;
    config.rule_set_version = rules.version;
    config.feature_schema_version = schema.version();
    config.max_cycle_age = std::max(std::chrono::milliseconds{5000}, options.interval * 5);
    api::ApiService service(*store, board, logger, metrics, config);

    std::unique_ptr<srv::CycleSource> source;
    if (options.source == srv::SourceKind::kPostgres) {
        source = std::make_unique<srv::PgCycleSource>(options.nodes, options.monitor_credentials,
                                                      options.connect, options.max_age);
    } else {
        source = std::make_unique<srv::SyntheticCycleSource>(options.synthetic_seed);
        logger.warn("monitor", "synthetic source: every value is generated, not measured",
                    {{"seed", options.synthetic_seed}});
    }
    srv::MonitorLoop loop(std::move(source), recorder, board, logger, metrics, options.interval);

    auto& app = drogon::app();
    app.setLogLevel(trantor::Logger::kWarn)
        .setThreadNum(options.http_threads)
        .setClientMaxBodySize(64UL * 1024UL)
        .setIdleConnectionTimeout(60)
        .enableServerHeader(false)
        .addListener(options.listen_host, options.listen_port)
        .setDefaultHandler([&service](const drogon::HttpRequestPtr& request,
                                      std::function<void(const drogon::HttpResponsePtr&)>&& reply) {
            const auto respond = std::move(reply);
            auto converted = srv::to_api_request(request);
            if (!converted) {
                respond(srv::to_drogon_response(
                    api::error_response(400, "invalid_parameter", "malformed query string", "")));
                return;
            }
            respond(srv::to_drogon_response(service.handle(*converted)));
        });
    // SIGTERM and SIGINT are blocked in every thread (see main) and taken by this thread with
    // sigwait (portable to macOS, which has no sigtimedwait), so the shutdown code runs as
    // ordinary code, not inside a signal handler. Drogon's own handler is disabled: it logs and
    // calls back from signal context, which is not async-signal-safe (ThreadSanitizer reports
    // it).
    app.disableSigtermHandling();
    std::jthread signal_waiter;
    app.registerBeginningAdvice([&logger, &app, &service, &signal_waiter] {
        signal_waiter = std::jthread([&logger, &app, &service](const std::stop_token& stop) {
            const sigset_t signals = shutdown_signals();
            int sig = 0;
            if (sigwait(&signals, &sig) != 0 || stop.stop_requested()) {
                return;  // woken by run() itself on the way out
            }
            logger.info("server", "shutdown requested; draining", {{"signal", sig}});
            service.begin_shutdown();
            app.quit();
        });
        // Drogon returns one listener per I/O thread for the same address; log each once.
        std::set<std::string> addresses;
        for (const auto& address : app.getListeners()) {
            addresses.insert(address.toIpPort());
        }
        for (const auto& address : addresses) {
            logger.info("server", "listening", {{"address", address}});
        }
    });

    loop.start();
    app.run();  // returns after quit()
    if (signal_waiter.joinable()) {
        // If the loop ended without a signal, wake the waiter; if it already handled one, the
        // extra signal goes to a finished thread and is discarded.
        signal_waiter.request_stop();
        pthread_kill(signal_waiter.native_handle(), SIGINT);
        signal_waiter.join();
    }
    loop.stop();
    logger.info("server", "stopped",
                {{"incidents_opened", recorder.counters().opened},
                 {"pending_incident_writes", static_cast<std::uint64_t>(recorder.pending())}});
    return kExitOk;
}
// NOLINTEND(misc-const-correctness)

}  // namespace

int main(int argc, char** argv) {
    // Block the shutdown signals before any thread exists, so every thread inherits the mask and
    // only the waiter thread in run() receives them.
    const sigset_t signals = shutdown_signals();
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    try {
        const std::vector<std::string_view> args(argv + 1, argv + argc);
        const std::string_view program = argc > 0 ? argv[0] : "faulttwind";
        const auto env = [](std::string_view name) -> std::optional<std::string> {
            const std::string key(name);
            // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread is started
            const char* value = std::getenv(key.c_str());
            return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
        };
        const auto parsed = srv::parse_daemon_options(args, env);
        if (std::holds_alternative<srv::HelpRequested>(parsed)) {
            std::cout << srv::daemon_usage(program);
            return kExitOk;
        }
        if (const auto* usage = std::get_if<srv::UsageError>(&parsed)) {
            std::cerr << "error: " << usage->message << "\n\n" << srv::daemon_usage(program);
            return kExitUsage;
        }
        return run(std::get<srv::DaemonOptions>(parsed));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return kExitStartup;
    } catch (...) {
        std::cerr << "fatal: unknown exception\n";
        return kExitStartup;
    }
}
