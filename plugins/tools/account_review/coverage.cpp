#include "coverage.hpp"
#include <asterion/foundation/error.hpp>

namespace asterion {

AccountObservationReview
review_account_observation(AccountObservationCoverage coverage, std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "account observation review cancelled");
    AccountObservationReview result;
    result.coverage = coverage;
    result.limitations = {
        "complete_broker_history",
        "cashflow_history",
        "settlement_evidence",
        "valuation_history",
        "component_business_times",
        "funds_currency"
    };
    if (!coverage.broker_ready)
        result.limitations.push_back("broker_not_ready");
    if (!coverage.positions_reconciled)
        result.limitations.push_back("known_fills_not_reconciled");
    if (!coverage.funds_present)
        result.limitations.push_back("funds_observation_missing");
    if (coverage.recovery_required)
        result.limitations.push_back("source_requires_recovery");
    if (coverage.unconfirmed_orders)
        result.limitations.push_back("unconfirmed_order_intents");
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "account observation review cancelled");
    return result;
}

} // namespace asterion
