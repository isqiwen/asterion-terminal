#pragma once
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace asterion {

struct NoticeAvailability
{
    std::optional<std::int64_t> source_published_at_ms;
    std::int64_t acquired_at_ms = 0, processed_at_ms = 0, verified_at_ms = 0, stored_at_ms = 0,
                 available_at_ms = 0, as_of_ms = 0;
    bool declared_strict_history = false;
};

struct NoticeAvailabilityReview
{
    bool published_by_cutoff = false, strict_history_eligible = false;
    std::vector<std::string> limitations;
};

NoticeAvailabilityReview
review_notice_availability(const NoticeAvailability&, std::stop_token = {});

} // namespace asterion
