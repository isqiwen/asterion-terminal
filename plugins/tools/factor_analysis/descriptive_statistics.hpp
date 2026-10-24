#pragma once
#include "feature_expression.hpp"
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>
#include <string>

namespace asterion {

enum class DistributionRole
{
    full_sample,
    development,
    holdout
};
enum class DistributionMetric
{
    feature,
    forward_return
};
enum class DistributionReason
{
    valid,
    warmup,
    zero_denominator,
    non_finite,
    unknown_label,
    crossed_label,
    other_role
};
enum class StatisticState
{
    defined,
    no_valid_samples,
    insufficient_samples,
    non_finite
};

// Ordered, validated original evidence. A feature's missing reason is independent
// of whether its future label exists or qualifies for paired evaluation.
struct DistributionObservation
{
    FeatureValue feature;
    std::optional<double> forward_return;
    DistributionRole role = DistributionRole::full_sample;
    bool label_crosses_boundary = false;
};

struct DistributionSelection
{
    DistributionMetric metric = DistributionMetric::feature;
    DistributionRole role = DistributionRole::full_sample;
    std::size_t begin = 0, end = 0, bins = 10;
};

struct DistributionRow
{
    std::size_t index;
    std::optional<double> value;
    DistributionReason reason;
    std::optional<std::size_t> bin;
};

struct DistributionBin
{
    double lower, upper;
    std::size_t count;
};

struct Distribution
{
    std::size_t allowed = 0, valid = 0;
    std::array<std::size_t, 7> reasons{};
    std::optional<double> mean, standard_deviation, minimum, q25, median, q75, maximum;
    StatisticState mean_state = StatisticState::no_valid_samples;
    StatisticState standard_deviation_state = StatisticState::no_valid_samples;
    std::vector<DistributionRow> rows;
    std::vector<DistributionBin> bins;
};

// Explicit roles over actual source observations. No value or future label is
// an input to membership, and no calendar rows are fabricated.
std::vector<DistributionRole> observation_roles(
    std::size_t count,
    std::optional<std::size_t> development_end,
    std::stop_token = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});

// Sample standard deviation (n-1), exact Type7 quantiles, equal-width bins with
// a closed final edge. The returned rows and bins use the same valid set.
Distribution describe_distribution(
    std::span<const DistributionObservation>,
    DistributionSelection,
    std::stop_token = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});

struct DistributionGroup
{
    std::string key;
    Distribution distribution;
};

struct GroupedDistribution
{
    Distribution whole;
    std::vector<DistributionGroup> groups;
};

// Group by the original supplied day, never by presentation timestamps.
// All groups share the whole report's bin edges, but retain their own exact
// sample statistics. Row indices still name the original observations.
GroupedDistribution describe_distribution_groups(
    std::span<const DistributionObservation>,
    std::span<const std::string> trading_days,
    DistributionSelection,
    std::stop_token = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});

} // namespace asterion
