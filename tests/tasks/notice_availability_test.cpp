#include "availability.hpp"
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <algorithm>
using namespace asterion;

namespace {

NoticeAvailability input()
{
    NoticeAvailability v;
    v.source_published_at_ms = 100;
    v.acquired_at_ms = 200;
    v.processed_at_ms = 300;
    v.verified_at_ms = 400;
    v.stored_at_ms = 500;
    v.available_at_ms = 600;
    v.as_of_ms = 600;
    v.declared_strict_history = true;
    return v;
}

bool has(const NoticeAvailabilityReview& r, const std::string& s)
{
    return std::ranges::find(r.limitations, s) != r.limitations.end();
}

} // namespace

TEST(
    NoticeAvailability,
    ActualAvailabilityIncludingReviewAndStorageGatesStrictHistoricalEligibility)
{
    auto v = input();
    auto r = review_notice_availability(v);
    EXPECT_TRUE(r.published_by_cutoff);
    EXPECT_TRUE(r.strict_history_eligible);
    EXPECT_TRUE(has(r, "no_prediction_validation"));
    v.as_of_ms = 599;
    r = review_notice_availability(v);
    EXPECT_FALSE(r.published_by_cutoff);
    EXPECT_FALSE(r.strict_history_eligible);
    EXPECT_TRUE(has(r, "publication_after_cutoff"));
}

TEST(NoticeAvailability, UnknownSourceTimeAndPostHocPurposeNeverBecomeStrictHistory)
{
    auto v = input();
    v.source_published_at_ms.reset();
    auto r = review_notice_availability(v);
    EXPECT_TRUE(r.published_by_cutoff);
    EXPECT_FALSE(r.strict_history_eligible);
    EXPECT_TRUE(has(r, "source_publication_time_unknown"));
    v.source_published_at_ms = 100;
    v.declared_strict_history = false;
    r = review_notice_availability(v);
    EXPECT_FALSE(r.strict_history_eligible);
    EXPECT_TRUE(has(r, "post_hoc_publication"));
    v.source_published_at_ms = 201;
    r = review_notice_availability(v);
    EXPECT_TRUE(has(r, "source_publication_time_inconsistent"));
}

TEST(NoticeAvailability, ClockDisorderAndCancellationDoNotProduceAReport)
{
    auto v = input();
    v.stored_at_ms = 399;
    EXPECT_THROW(review_notice_availability(v), std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(review_notice_availability(input(), stop.get_token()), Error);
}
