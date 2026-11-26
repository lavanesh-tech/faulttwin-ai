#include "faulttwin/concurrency/queue_types.hpp"

namespace faulttwin::concurrency {

std::string_view to_string(OverloadPolicy policy) noexcept {
    switch (policy) {
        case OverloadPolicy::kBlock:
            return "block";
        case OverloadPolicy::kRejectNewest:
            return "reject-newest";
        case OverloadPolicy::kDropOldest:
            return "drop-oldest";
    }
    return "invalid";
}

std::optional<OverloadPolicy> parse_overload_policy(std::string_view text) noexcept {
    for (const OverloadPolicy policy :
         {OverloadPolicy::kBlock, OverloadPolicy::kRejectNewest, OverloadPolicy::kDropOldest}) {
        if (text == to_string(policy)) {
            return policy;
        }
    }
    return std::nullopt;
}

std::string_view to_string(PushStatus status) noexcept {
    switch (status) {
        case PushStatus::kAccepted:
            return "accepted";
        case PushStatus::kAcceptedDisplacedOldest:
            return "accepted-displaced-oldest";
        case PushStatus::kRejectedFull:
            return "rejected-full";
        case PushStatus::kTimedOut:
            return "timed-out";
        case PushStatus::kCancelled:
            return "cancelled";
        case PushStatus::kClosed:
            return "closed";
    }
    return "invalid";
}

std::string_view to_string(PopStatus status) noexcept {
    switch (status) {
        case PopStatus::kItem:
            return "item";
        case PopStatus::kTimedOut:
            return "timed-out";
        case PopStatus::kCancelled:
            return "cancelled";
        case PopStatus::kClosed:
            return "closed";
    }
    return "invalid";
}

}  // namespace faulttwin::concurrency
