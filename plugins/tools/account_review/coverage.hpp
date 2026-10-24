#pragma once
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace asterion {

// Counts refer to records in one observed cut, never complete historical totals.
struct AccountObservationCoverage
{
    std::uint32_t position_records = 0, order_records = 0, trade_records = 0,
                  unconfirmed_orders = 0;
    bool broker_ready = false, positions_reconciled = false, funds_present = false,
         recovery_required = false;
};

struct AccountObservationReview
{
    AccountObservationCoverage coverage;
    // This source is an observation, lacking cashflow, settlement and valuation history.
    bool evidence_sufficient = false;
    std::vector<std::string> limitations;
};

AccountObservationReview
    review_account_observation(AccountObservationCoverage, std::stop_token = {});

} // namespace asterion
