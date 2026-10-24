#include "availability.hpp"
#include <asterion/foundation/error.hpp>
#include <stdexcept>

namespace asterion {

NoticeAvailabilityReview
review_notice_availability(const NoticeAvailability& v, std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "notice availability review cancelled");
    if (v.as_of_ms <= 0 || v.acquired_at_ms <= 0 || v.processed_at_ms < v.acquired_at_ms
        || v.verified_at_ms < v.processed_at_ms || v.stored_at_ms < v.verified_at_ms
        || v.available_at_ms < v.stored_at_ms)
        throw std::invalid_argument(
            "notice availability review requires ordered actual evidence times");
    NoticeAvailabilityReview result;
    result.published_by_cutoff = v.available_at_ms <= v.as_of_ms;
    const bool known = v.source_published_at_ms && *v.source_published_at_ms > 0
        && *v.source_published_at_ms <= v.acquired_at_ms;
    result.strict_history_eligible =
        result.published_by_cutoff && known && v.declared_strict_history;
    result.limitations = {"fixed_announcement_evidence_only", "no_prediction_validation"};
    if (!v.source_published_at_ms)
        result.limitations.push_back("source_publication_time_unknown");
    else if (!known)
        result.limitations.push_back("source_publication_time_inconsistent");
    if (!v.declared_strict_history)
        result.limitations.push_back("post_hoc_publication");
    if (!result.published_by_cutoff)
        result.limitations.push_back("publication_after_cutoff");
    return result;
}

} // namespace asterion
