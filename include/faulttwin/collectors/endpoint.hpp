#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace faulttwin::collectors {

/// Where one monitored PostgreSQL node is reachable. `node` is the inventory name; for replicas
/// it should equal the server's cluster_name, which the primary sees as application_name.
struct PgEndpoint {
    std::string node;
    std::string host;
    std::uint16_t port{5432};
    friend bool operator==(const PgEndpoint&, const PgEndpoint&) = default;
};

struct EndpointParseError {
    std::string message;
};

/// Parses "name=host:port,name=host:port". Names follow the NodeId rules; ports are 1-65535;
/// names must be unique; at least one entry is required. Hosts are not resolved here.
[[nodiscard]] std::variant<std::vector<PgEndpoint>, EndpointParseError> parse_endpoint_list(
    std::string_view text);

}  // namespace faulttwin::collectors
