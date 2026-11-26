#include "faulttwin/collectors/endpoint.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

#include "faulttwin/telemetry/node_id.hpp"

namespace faulttwin::collectors {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace

std::variant<std::vector<PgEndpoint>, EndpointParseError> parse_endpoint_list(
    std::string_view text) {
    std::vector<PgEndpoint> endpoints;
    std::string_view rest = text;
    while (true) {
        const auto comma = rest.find(',');
        const std::string_view entry = trim(rest.substr(0, comma));
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            return EndpointParseError{"expected name=host:port, got '" + std::string(entry) + "'"};
        }
        const std::string_view name = entry.substr(0, equals);
        const std::string_view address = entry.substr(equals + 1);
        if (const auto error = telemetry::check_node_id(name)) {
            return EndpointParseError{"invalid node name '" + std::string(name) +
                                      "': " + std::string(telemetry::to_string(*error))};
        }
        const auto colon = address.rfind(':');
        if (colon == std::string_view::npos || colon == 0) {
            return EndpointParseError{"expected host:port for node '" + std::string(name) + "'"};
        }
        const std::string_view host = address.substr(0, colon);
        const std::string_view port_text = address.substr(colon + 1);
        unsigned int port = 0;
        const char* const last = port_text.data() + port_text.size();
        const auto [ptr, ec] = std::from_chars(port_text.data(), last, port);
        if (port_text.empty() || ec != std::errc{} || ptr != last || port == 0 || port > 65535U) {
            return EndpointParseError{"invalid port '" + std::string(port_text) + "' for node '" +
                                      std::string(name) + "'"};
        }
        if (std::ranges::any_of(endpoints, [&](const PgEndpoint& e) { return e.node == name; })) {
            return EndpointParseError{"duplicate node name '" + std::string(name) + "'"};
        }
        endpoints.push_back(
            PgEndpoint{std::string(name), std::string(host), static_cast<std::uint16_t>(port)});
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return endpoints;
}

}  // namespace faulttwin::collectors
