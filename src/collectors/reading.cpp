#include "faulttwin/collectors/reading.hpp"

namespace faulttwin::collectors {

std::string_view to_string(Quality quality) noexcept {
    switch (quality) {
        case Quality::kOk:
            return "ok";
        case Quality::kMissing:
            return "missing";
        case Quality::kStale:
            return "stale";
        case Quality::kInvalid:
            return "invalid";
        case Quality::kNotApplicable:
            return "not_applicable";
    }
    return "unknown";
}

}  // namespace faulttwin::collectors
