#include "faulttwin/collectors/libpq_session.hpp"

#include <libpq-fe.h>
#include <poll.h>

#include <array>
#include <cerrno>
#include <string>
#include <utility>
#include <vector>

namespace faulttwin::collectors {
namespace {

struct ConnDeleter {
    void operator()(PGconn* conn) const noexcept { PQfinish(conn); }
};
struct ResultDeleter {
    void operator()(PGresult* result) const noexcept { PQclear(result); }
};
using ConnPtr = std::unique_ptr<PGconn, ConnDeleter>;
using ResultPtr = std::unique_ptr<PGresult, ResultDeleter>;

/// libpq reports connection failures as text only. These substrings are stable libpq/server
/// messages in the C/English locale that the lab uses.
SqlErrorKind classify_connect_error(std::string_view message) {
    if (message.find("password authentication failed") != std::string_view::npos ||
        message.find("no password supplied") != std::string_view::npos ||
        message.find("authentication failed") != std::string_view::npos) {
        return SqlErrorKind::kAuthenticationFailed;
    }
    if (message.find("timeout expired") != std::string_view::npos) {
        return SqlErrorKind::kTimeout;
    }
    return SqlErrorKind::kConnectionFailed;
}

SqlError error_from_result(const PGresult* result, PGconn* conn) {
    const char* state = result != nullptr ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : nullptr;
    const char* primary =
        result != nullptr ? PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY) : nullptr;
    const std::string message = primary != nullptr ? primary : PQerrorMessage(conn);
    SqlError error;
    error.sqlstate = state != nullptr ? state : "";
    error.kind = error.sqlstate.empty() ? SqlErrorKind::kConnectionFailed
                                        : classify_sqlstate(error.sqlstate);
    error.message = sanitize_message(message);
    return error;
}

}  // namespace

struct LibpqSession::Impl {
    ConnPtr conn;
    std::chrono::milliseconds query_timeout{};
    bool broken{false};
};

LibpqSession::LibpqSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LibpqSession::~LibpqSession() = default;

std::variant<std::unique_ptr<LibpqSession>, SqlError> LibpqSession::connect(
    const PgEndpoint& endpoint, const PgCredentials& credentials, const ConnectOptions& options) {
    const std::string port = std::to_string(endpoint.port);
    const std::string connect_timeout = std::to_string(options.connect_timeout.count());
    // Server-side guards for every session, independent of the role's own defaults.
    const std::string session_options =
        "-c default_transaction_read_only=on -c statement_timeout=" +
        std::to_string(options.statement_timeout.count()) + " -c lock_timeout=1000";

    const std::array<const char*, 11> keywords{"host",
                                               "port",
                                               "user",
                                               "password",
                                               "dbname",
                                               "connect_timeout",
                                               "application_name",
                                               "options",
                                               "client_encoding",
                                               "sslmode",
                                               nullptr};
    const std::array<const char*, 11> values{endpoint.host.c_str(),
                                             port.c_str(),
                                             credentials.user.c_str(),
                                             credentials.password.c_str(),
                                             credentials.dbname.c_str(),
                                             connect_timeout.c_str(),
                                             options.application_name.c_str(),
                                             session_options.c_str(),
                                             "UTF8",
                                             "prefer",
                                             nullptr};
    ConnPtr conn(PQconnectdbParams(keywords.data(), values.data(), 0));
    if (!conn) {
        return SqlError{
            SqlErrorKind::kConnectionFailed, {}, "libpq could not allocate a connection"};
    }
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        const std::string message = sanitize_message(PQerrorMessage(conn.get()));
        return SqlError{classify_connect_error(message), {}, message};
    }
    auto impl = std::make_unique<Impl>();
    impl->conn = std::move(conn);
    impl->query_timeout = options.query_timeout;
    return std::unique_ptr<LibpqSession>(new LibpqSession(std::move(impl)));
}

bool LibpqSession::usable() const noexcept {
    return !impl_->broken && PQstatus(impl_->conn.get()) == CONNECTION_OK;
}

namespace {

SqlTable table_from_result(const PGresult* result) {
    SqlTable table;
    const int columns = PQnfields(result);
    const int rows = PQntuples(result);
    table.columns.reserve(static_cast<std::size_t>(columns));
    for (int c = 0; c < columns; ++c) {
        table.columns.emplace_back(PQfname(result, c));
    }
    table.rows.reserve(static_cast<std::size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        std::vector<std::optional<std::string>> row;
        row.reserve(static_cast<std::size_t>(columns));
        for (int c = 0; c < columns; ++c) {
            if (PQgetisnull(result, r, c) != 0) {
                row.emplace_back(std::nullopt);
            } else {
                row.emplace_back(std::string(PQgetvalue(result, r, c),
                                             static_cast<std::size_t>(PQgetlength(result, r, c))));
            }
        }
        table.rows.push_back(std::move(row));
    }
    return table;
}

/// Waits until libpq has a complete result or the deadline passes. Returns an error when the
/// deadline expired or the socket failed.
std::optional<SqlError> wait_until_ready(PGconn* conn,
                                         std::chrono::steady_clock::time_point deadline) {
    while (PQisBusy(conn) != 0) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return SqlError{SqlErrorKind::kTimeout, {}, "client-side query deadline expired"};
        }
        pollfd fd{PQsocket(conn), POLLIN, 0};
        const int ready = ::poll(&fd, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0 || PQconsumeInput(conn) == 0) {
            return SqlError{
                SqlErrorKind::kConnectionFailed, {}, sanitize_message(PQerrorMessage(conn))};
        }
    }
    return std::nullopt;
}

/// Collects every pending result; the first one decides the outcome (single statements only).
std::optional<SqlOutcome> read_results(PGconn* conn) {
    std::optional<SqlOutcome> outcome;
    while (true) {
        const ResultPtr result(PQgetResult(conn));
        if (!result) {
            return outcome;
        }
        if (outcome) {
            continue;
        }
        const ExecStatusType status = PQresultStatus(result.get());
        if (status == PGRES_TUPLES_OK) {
            outcome = table_from_result(result.get());
        } else if (status == PGRES_COMMAND_OK) {
            outcome = SqlTable{};
        } else {
            outcome = error_from_result(result.get(), conn);
        }
    }
}

}  // namespace

SqlOutcome LibpqSession::query(std::string_view sql) {
    PGconn* const conn = impl_->conn.get();
    if (!usable()) {
        return SqlError{SqlErrorKind::kConnectionFailed, {}, "connection is not usable"};
    }
    const std::string statement(sql);
    if (PQsendQueryParams(conn, statement.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 0) == 0) {
        impl_->broken = PQstatus(conn) != CONNECTION_OK;
        return SqlError{
            SqlErrorKind::kConnectionFailed, {}, sanitize_message(PQerrorMessage(conn))};
    }
    if (auto error =
            wait_until_ready(conn, std::chrono::steady_clock::now() + impl_->query_timeout)) {
        // A late result may still arrive; the connection cannot be trusted for reuse.
        impl_->broken = true;
        return std::move(*error);
    }
    auto outcome = read_results(conn);
    if (PQstatus(conn) != CONNECTION_OK) {
        impl_->broken = true;
    }
    if (!outcome) {
        return SqlError{
            SqlErrorKind::kConnectionFailed, {}, sanitize_message(PQerrorMessage(conn))};
    }
    return std::move(*outcome);
}

PgNodeCollector::PgNodeCollector(PgEndpoint endpoint, PgCredentials credentials,
                                 ConnectOptions options, CollectionPolicy policy,
                                 CollectorClock clock)
    : endpoint_(std::move(endpoint)),
      credentials_(std::move(credentials)),
      options_(std::move(options)),
      policy_(policy),
      clock_(std::move(clock)) {}

PgNodeSnapshot PgNodeCollector::collect() {
    const Timestamp attempted_at = clock_.wall();
    Reading<double> connect_latency = Reading<double>::not_applicable("connection reused");
    if (!session_ || !session_->usable()) {
        session_.reset();
        const SteadyTime start = clock_.steady();
        auto connected = LibpqSession::connect(endpoint_, credentials_, options_);
        if (auto* error = std::get_if<SqlError>(&connected)) {
            return unreachable_snapshot(endpoint_.node, attempted_at, *error, clock_);
        }
        session_ = std::move(std::get<std::unique_ptr<LibpqSession>>(connected));
        connect_latency = Reading<double>::ok(
            std::chrono::duration<double, std::milli>(clock_.steady() - start).count());
    }
    PgNodeSnapshot snapshot =
        collect_pg_node(*session_, endpoint_.node, clock_, policy_, std::move(connect_latency));
    snapshot.attempted_at = attempted_at;
    if (!session_->usable()) {
        session_.reset();
        // The connection broke during collection (server stopped, deadline expired). If not
        // even the role could be read, nothing about the node is known: report it down.
        // Otherwise keep what was collected (the node is degraded); the next cycle reconnects.
        if (snapshot.role == NodeRole::kUnknown && !snapshot.errors.empty()) {
            return unreachable_snapshot(endpoint_.node, attempted_at, snapshot.errors.front(),
                                        clock_);
        }
    }
    return snapshot;
}

}  // namespace faulttwin::collectors
