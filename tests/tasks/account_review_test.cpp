#include "coverage.hpp"
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <algorithm>
using namespace asterion;

TEST(AccountObservationReview, ReadyWithObservedFundsStillCannotProveHistoryOrPerformance)
{
    AccountObservationCoverage observed;
    observed.broker_ready = observed.positions_reconciled = observed.funds_present = true;
    observed.position_records = 2;
    observed.order_records = 3;
    observed.trade_records = 4;
    const auto result = review_account_observation(observed);
    EXPECT_FALSE(result.evidence_sufficient);
    EXPECT_EQ(result.coverage.position_records, 2U);
    EXPECT_EQ(result.coverage.order_records, 3U);
    EXPECT_EQ(result.coverage.trade_records, 4U);
    EXPECT_NE(std::ranges::find(result.limitations, "cashflow_history"), result.limitations.end());
    EXPECT_NE(
        std::ranges::find(result.limitations, "settlement_evidence"),
        result.limitations.end());
    EXPECT_EQ(
        std::ranges::find(result.limitations, "funds_observation_missing"),
        result.limitations.end());
    EXPECT_EQ(
        std::ranges::find(result.limitations, "known_fills_not_reconciled"),
        result.limitations.end());
}

TEST(AccountObservationReview, MissingFactsAndUnknownOrdersAreNotReplacedBySuccessfulZeroValues)
{
    AccountObservationCoverage observed;
    observed.recovery_required = true;
    observed.unconfirmed_orders = 3;
    const auto result = review_account_observation(observed);
    EXPECT_FALSE(result.evidence_sufficient);
    EXPECT_FALSE(result.coverage.funds_present);
    EXPECT_EQ(result.coverage.unconfirmed_orders, 3U);
    for (const auto* gap :
        {"broker_not_ready",
            "known_fills_not_reconciled",
            "funds_observation_missing",
            "source_requires_recovery",
            "unconfirmed_order_intents"})
        EXPECT_NE(std::ranges::find(result.limitations, gap), result.limitations.end());
}

TEST(AccountObservationReview, CancellationProducesNoInventedConclusion)
{
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(review_account_observation({}, stop.get_token()), Error);
}
