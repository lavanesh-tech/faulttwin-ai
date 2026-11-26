#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace faulttwin::collectors {

/// Broad failure classes. Collectors treat them differently (e.g. a permission error on one
/// view degrades the node, a connection failure marks it down).
enum class SqlErrorKind : std::uint8_t {
    kConnectionFailed,
    kAuthenticationFailed,
    kTimeout,           ///< client-side deadline or server statement_timeout
    kPermissionDenied,  ///< SQLSTATE 42501
    kUndefinedObject,   ///< view/function/extension does not exist (42P01, 42883, 42704)
    kReadOnly,          ///< write attempted in a read-only transaction (25006)
    kQueryFailed,       ///< any other server error
    kUnexpectedResult,  ///< the result did not have the expected shape
};

[[nodiscard]] std::string_view to_string(SqlErrorKind kind) noexcept;

/// Maps a five-character SQLSTATE to a kind. Unknown codes map to kQueryFailed.
[[nodiscard]] SqlErrorKind classify_sqlstate(std::string_view sqlstate) noexcept;

struct SqlError {
    SqlErrorKind kind{SqlErrorKind::kQueryFailed};
    std::string sqlstate;  ///< empty when the error did not come from the server
    std::string message;   ///< single line, bounded length; never contains credentials
};

/// A text-format result. A NULL field is std::nullopt, never an empty string.
struct SqlTable {
    std::vector<std::string> columns;
    std::vector<std::vector<std::optional<std::string>>> rows;
};

using SqlOutcome = std::variant<SqlTable, SqlError>;

/// Minimal query interface. The libpq adapter implements it for real servers; unit tests use a
/// scripted fake, so every parsing and classification path is testable without a database.
class SqlSession {
  public:
    SqlSession() = default;
    SqlSession(const SqlSession&) = delete;
    SqlSession& operator=(const SqlSession&) = delete;
    SqlSession(SqlSession&&) = delete;
    SqlSession& operator=(SqlSession&&) = delete;
    virtual ~SqlSession() = default;

    /// Runs one statement without parameters and returns all rows as text.
    [[nodiscard]] virtual SqlOutcome query(std::string_view sql) = 0;
};

/// Collapses a message to one line and at most `max_length` bytes.
[[nodiscard]] std::string sanitize_message(std::string_view message, std::size_t max_length = 240);

}  // namespace faulttwin::collectors
