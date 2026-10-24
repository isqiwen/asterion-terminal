#include "descriptive_statistics.hpp"
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <numeric>
using namespace asterion;

namespace {

std::vector<DistributionObservation> observations(
    std::initializer_list<std::optional<double>> values)
{
    std::vector<DistributionObservation> rows;
    for (const auto value : values)
        rows.push_back(
            {{value, value ? FeatureUndefined::none : FeatureUndefined::warmup},
                {},
                DistributionRole::full_sample,
                false});
    return rows;
}

Distribution describe(const std::vector<DistributionObservation>& rows, std::size_t bins = 4)
{
    return describe_distribution(
        rows,
        {DistributionMetric::feature, DistributionRole::full_sample, 0, rows.size(), bins});
}

} // namespace

TEST(DescriptiveStatistics, MissingIsNotZeroAndType7QuantilesUseTheSameValidRowsAsBins)
{
    const auto result = describe(observations({.01, -.01, std::nullopt, .02}));
    ASSERT_EQ(result.valid, 3U);
    EXPECT_EQ(result.allowed, 4U);
    EXPECT_NEAR(result.mean.value(), .02 / 3, 1e-15);
    EXPECT_NEAR(result.standard_deviation.value(), std::sqrt(7.0 / 3) * .01, 1e-15);
    EXPECT_DOUBLE_EQ(result.q25.value(), 0);
    EXPECT_DOUBLE_EQ(result.median.value(), .01);
    EXPECT_DOUBLE_EQ(result.q75.value(), .015);
    EXPECT_FALSE(result.rows[2].value);
    EXPECT_FALSE(result.rows[2].bin);
    EXPECT_EQ(result.rows[2].reason, DistributionReason::warmup);
    std::size_t bins = 0;
    for (const auto& bin : result.bins)
        bins += bin.count;
    EXPECT_EQ(bins, 3U);
    EXPECT_EQ(result.reasons[static_cast<std::size_t>(DistributionReason::valid)], 3U);
}

TEST(DescriptiveStatistics, EmptySingletonAndConstantZeroHaveDifferentStatisticalStates)
{
    const auto empty = describe({});
    EXPECT_FALSE(empty.mean);
    EXPECT_TRUE(empty.bins.empty());
    EXPECT_EQ(empty.mean_state, StatisticState::no_valid_samples);
    const auto single = describe(observations({0.0}));
    EXPECT_DOUBLE_EQ(single.mean.value(), 0);
    EXPECT_FALSE(single.standard_deviation);
    EXPECT_EQ(single.standard_deviation_state, StatisticState::insufficient_samples);
    const auto constant = describe(observations({0.0, 0.0, 0.0}));
    EXPECT_DOUBLE_EQ(constant.standard_deviation.value(), 0);
    ASSERT_EQ(constant.bins.size(), 1U);
    EXPECT_EQ(constant.bins[0].count, 3U);
}

TEST(DescriptiveStatistics, FeatureEligibilityNeverDependsOnFutureLabelsOrPairedInclusion)
{
    std::vector<DistributionObservation> source(90);
    for (std::size_t i = 0; i < 90; ++i) {
        auto& row = source[i];
        row.role = i < 45 ? DistributionRole::development : DistributionRole::holdout;
        row.feature = i < 2 ? FeatureValue{}
                            : FeatureValue{static_cast<double>(i) / 100, FeatureUndefined::none};
        if (i < 87)
            row.forward_return = .01;
        row.label_crosses_boundary = i >= 42 && i < 45;
    }
    const DistributionSelection
        features{DistributionMetric::feature, DistributionRole::development, 0, 90, 10};
    const auto result = describe_distribution(source, features);
    EXPECT_EQ(result.allowed, 45U);
    EXPECT_EQ(result.valid, 43U);
    EXPECT_TRUE(result.rows[42].value);
    EXPECT_FALSE(result.rows[45].value);
    EXPECT_FALSE(result.rows[45].bin);
    EXPECT_EQ(result.rows[45].reason, DistributionReason::other_role);
    auto returns = features;
    returns.metric = DistributionMetric::forward_return;
    const auto labels = describe_distribution(source, returns);
    EXPECT_EQ(labels.valid, 42U);
    EXPECT_TRUE(labels.rows[0].value);
    EXPECT_EQ(labels.rows[42].reason, DistributionReason::crossed_label);
    for (auto& row : source) {
        row.forward_return.reset();
        row.label_crosses_boundary = true;
    }
    const auto changed = describe_distribution(source, features);
    EXPECT_EQ(changed.valid, result.valid);
    EXPECT_EQ(changed.mean, result.mean);
    auto forbidden = features;
    forbidden.role = DistributionRole::holdout;
    EXPECT_THROW(describe_distribution(source, forbidden), std::invalid_argument);
}

TEST(DescriptiveStatistics, FiniteExtremesRemainVisibleAndOverflowIsNotZero)
{
    const auto large = describe(observations({1e308, 1e308}));
    EXPECT_DOUBLE_EQ(large.mean.value(), 1e308);
    EXPECT_DOUBLE_EQ(large.standard_deviation.value(), 0);
    const auto max = std::numeric_limits<double>::max();
    const auto extremes =
        describe(observations({-max, max, std::numeric_limits<double>::quiet_NaN()}));
    EXPECT_EQ(extremes.valid, 2U);
    EXPECT_DOUBLE_EQ(extremes.mean.value(), 0);
    EXPECT_EQ(extremes.standard_deviation_state, StatisticState::non_finite);
    EXPECT_FALSE(extremes.standard_deviation);
    EXPECT_DOUBLE_EQ(extremes.minimum.value(), -max);
    EXPECT_DOUBLE_EQ(extremes.maximum.value(), max);
    EXPECT_EQ(extremes.rows[2].reason, DistributionReason::non_finite);
    for (const auto& bin : extremes.bins) {
        EXPECT_TRUE(std::isfinite(bin.lower));
        EXPECT_TRUE(std::isfinite(bin.upper));
    }
}

TEST(DescriptiveStatistics, BoundedCalculationStopsCooperativelyWithoutReturningAPartialReport)
{
    std::vector<DistributionObservation> source(200000);
    for (auto& row : source)
        row.feature = {.01, FeatureUndefined::none};
    std::stop_source stop;
    EXPECT_THROW(
        describe_distribution(
            source,
            {DistributionMetric::feature, DistributionRole::full_sample, 0, source.size(), 64},
            stop.get_token(),
            [&](auto completed, auto) {
                if (completed >= 4096)
                    stop.request_stop();
            }),
        std::runtime_error);
    EXPECT_TRUE(stop.stop_requested());
    auto invalid = DistributionSelection{
        DistributionMetric::feature,
        DistributionRole::full_sample,
        0,
        source.size(),
        65
    };
    EXPECT_THROW(describe_distribution(source, invalid), std::invalid_argument);
}

TEST(DescriptiveStatistics, OriginalTradingDaysGroupNightAndDayWithoutLeakingHeldValues)
{
    auto source = observations({100, -1, 0, 2, -2, 4});
    source[4].role = source[5].role = DistributionRole::holdout;
    for (int i = 0; i < 4; ++i)
        source[i].role = DistributionRole::development;
    const std::vector<std::string> days =
        {"2026-09-25", "2026-09-25", "2026-09-25", "2026-09-28", "2026-09-28", "2026-09-28"};
    std::size_t done = 0, total = 0;
    const auto report = describe_distribution_groups(
        source,
        days,
        {DistributionMetric::feature, DistributionRole::development, 0, source.size(), 3},
        {},
        [&](auto completed, auto work) {
            done = completed;
            total = work;
        });
    EXPECT_EQ(done, 24U);
    EXPECT_EQ(total, 24U);
    ASSERT_EQ(report.groups.size(), 2U);
    EXPECT_EQ(report.groups[0].key, "2026-09-25");
    const auto& first = report.groups[0].distribution;
    EXPECT_EQ(first.valid, 3U);
    EXPECT_DOUBLE_EQ(*first.mean, 33);
    EXPECT_DOUBLE_EQ(*first.minimum, -1);
    EXPECT_DOUBLE_EQ(*first.median, 0);
    const auto& second = report.groups[1].distribution;
    EXPECT_EQ(second.rows.size(), 3U);
    EXPECT_EQ(second.allowed, 1U);
    EXPECT_EQ(second.valid, 1U);
    EXPECT_DOUBLE_EQ(*second.mean, 2);
    EXPECT_EQ(second.standard_deviation_state, StatisticState::insufficient_samples);
    EXPECT_EQ(second.rows[0].index, 3U);
    EXPECT_FALSE(second.rows[1].value);
    EXPECT_FALSE(second.rows[1].bin);
    for (std::size_t i = 0; i < report.whole.bins.size(); ++i) {
        EXPECT_DOUBLE_EQ(first.bins[i].lower, report.whole.bins[i].lower);
        EXPECT_DOUBLE_EQ(second.bins[i].upper, report.whole.bins[i].upper);
        EXPECT_EQ(first.bins[i].count + second.bins[i].count, report.whole.bins[i].count);
    }
    std::stop_source stopped;
    stopped.request_stop();
    EXPECT_THROW(
        describe_distribution_groups(
            source,
            days,
            {DistributionMetric::feature, DistributionRole::development, 0, source.size(), 3},
            stopped.get_token()),
        Error);
}
