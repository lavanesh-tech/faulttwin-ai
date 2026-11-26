#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include "faulttwin/collectors/endpoint.hpp"
#include "faulttwin/collectors/pg_snapshot.hpp"
#include "faulttwin/collectors/sql.hpp"

namespace faulttwin::collectors {

/// Credentials for the monitoring role. The password is held in memory only: it is passed to
/// libpq as a connection parameter, never placed on a command line, logged or printed.
struct PgCredentials {
    std::string user;
    std::string password;
    std::string dbname{"postgres"};
};

struct ConnectOptions {
    /// libpq connect_timeout (whole seconds; libpq treats values below 2 as 2).
    std::chrono::seconds connect_timeout{2};
    /// Client-side deadline per query. On expiry the connection is closed, because a server
    /// that does not answer (paused, partitioned) cannot be relied on to honour a cancel.
    std::chrono::milliseconds query_timeout{2000};
    /// Server-side statement_timeout set for the session as a second line of defence.
    std::chrono::milliseconds statement_timeout{1500};
    std::string application_name{"faulttwin-collector"};
};

/// Owns one libpq connection. Every session is forced read-only
/// (default_transaction_read_only=on) in addition to the role's own settings.
class LibpqSession final : public SqlSession {
  public:
    [[nodiscard]] static std::variant<std::unique_ptr<LibpqSession>, SqlError> connect(
        const PgEndpoint& endpoint, const PgCredentials& credentials,
        const ConnectOptions& options);

    LibpqSession(const LibpqSession&) = delete;
    LibpqSession& operator=(const LibpqSession&) = delete;
    LibpqSession(LibpqSession&&) = delete;
    LibpqSession& operator=(LibpqSession&&) = delete;
    ~LibpqSession() override;

    [[nodiscard]] SqlOutcome query(std::string_view sql) override;
    /// False after a connection-level failure or a client-side timeout.
    [[nodiscard]] bool usable() const noexcept;

  private:
    struct Impl;
    explicit LibpqSession(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

/// Keeps a connection to one node across cycles and reconnects when it breaks. One collector
/// per node; not thread-safe (each collector is used by one thread at a time).
class PgNodeCollector {
  public:
    PgNodeCollector(PgEndpoint endpoint, PgCredentials credentials, ConnectOptions options,
                    CollectionPolicy policy = {}, CollectorClock clock = CollectorClock::system());

    [[nodiscard]] PgNodeSnapshot collect();
    [[nodiscard]] const PgEndpoint& endpoint() const noexcept { return endpoint_; }

  private:
    PgEndpoint endpoint_;
    PgCredentials credentials_;
    ConnectOptions options_;
    CollectionPolicy policy_;
    CollectorClock clock_;
    std::unique_ptr<LibpqSession> session_;
};

}  // namespace faulttwin::collectors
