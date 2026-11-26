#include "faulttwin/collectors/sql.hpp"

namespace faulttwin::collectors {

std::string_view to_string(SqlErrorKind kind) noexcept {
    switch (kind) {
        case SqlErrorKind::kConnectionFailed:
            return "connection_failed";
        case SqlErrorKind::kAuthenticationFailed:
            return "authentication_failed";
        case SqlErrorKind::kTimeout:
            return "timeout";
        case SqlErrorKind::kPermissionDenied:
            return "permission_denied";
        case SqlErrorKind::kUndefinedObject:
            return "undefined_object";
        case SqlErrorKind::kReadOnly:
            return "read_only";
        case SqlErrorKind::kQueryFailed:
            return "query_failed";
        case SqlErrorKind::kUnexpectedResult:
            return "unexpected_result";
    }
    return "unknown";
}

SqlErrorKind classify_sqlstate(std::string_view sqlstate) noexcept {
    if (sqlstate.size() != 5) {
        return SqlErrorKind::kQueryFailed;
    }
    if (sqlstate == "42501") {
        return SqlErrorKind::kPermissionDenied;
    }
    if (sqlstate == "42P01" || sqlstate == "42883" || sqlstate == "42704") {
        return SqlErrorKind::kUndefinedObject;
    }
    if (sqlstate == "57014") {  // query_canceled, raised by statement_timeout
        return SqlErrorKind::kTimeout;
    }
    if (sqlstate == "25006") {
        return SqlErrorKind::kReadOnly;
    }
    if (sqlstate.starts_with("28")) {  // invalid_authorization_specification class
        return SqlErrorKind::kAuthenticationFailed;
    }
    // Class 08 (connection exception) and 57P01-57P03 (admin shutdown, crash shutdown,
    // cannot connect now) all mean the session is gone.
    if (sqlstate.starts_with("08") || sqlstate == "57P01" || sqlstate == "57P02" ||
        sqlstate == "57P03") {
        return SqlErrorKind::kConnectionFailed;
    }
    return SqlErrorKind::kQueryFailed;
}

std::string sanitize_message(std::string_view message, std::size_t max_length) {
    std::string out;
    out.reserve(message.size() < max_length ? message.size() : max_length);
    bool last_was_space = true;  // also trims leading whitespace
    for (const char c : message) {
        if (out.size() >= max_length) {
            break;
        }
        const auto uc = static_cast<unsigned char>(c);
        const bool space = c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (space) {
            if (!last_was_space) {
                out.push_back(' ');
            }
            last_was_space = true;
            continue;
        }
        // Drop other control characters; keep printable ASCII and UTF-8 bytes as they are.
        if (uc < 0x20U || uc == 0x7FU) {
            continue;
        }
        out.push_back(c);
        last_was_space = false;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

}  // namespace faulttwin::collectors
