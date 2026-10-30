#include "descriptive_statistics.hpp"
#include <asterion/foundation/error.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace asterion {
namespace {

DistributionReason feature_reason(FeatureUndefined state)
{
    switch (state) {
        case FeatureUndefined::none:
            return DistributionReason::valid;
        case FeatureUndefined::warmup:
            return DistributionReason::warmup;
        case FeatureUndefined::zero_denominator:
            return DistributionReason::zero_denominator;
        case FeatureUndefined::non_finite:
            return DistributionReason::non_finite;
    }
    throw std::logic_error("unknown feature state");
}

} // namespace

std::vector<DistributionRole> observation_roles(
    std::size_t count,
    std::optional<std::size_t> development_end,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress)
{
    if (count < 1 || count > 200000)
        throw std::invalid_argument("bar dataset requires 1..200000 bars and their trading days");
    if (development_end && (*development_end == 0 || *development_end >= count))
        throw std::invalid_argument(
            "sample development boundary is outside the actual observations");
    const auto advance = [&](std::size_t completed) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (progress)
            progress(completed, count);
    };
    advance(0);
    std::vector<DistributionRole> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(
            !development_end           ? DistributionRole::full_sample
                : i < *development_end ? DistributionRole::development
                                       : DistributionRole::holdout);
        if ((i + 1) % 4096 == 0)
            advance(i + 1);
    }
    advance(count);
    return result;
}

Distribution describe_distribution(
    std::span<const DistributionObservation> source,
    DistributionSelection selection,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress)
{
    if (source.size() > 200000 || selection.bins < 1 || selection.bins > 64)
        throw std::invalid_argument("distribution requires at most 200000 rows and 1..64 bins");
    if (selection.begin > selection.end || selection.end > source.size())
        throw std::invalid_argument("distribution range is outside its source");
    if (selection.role == DistributionRole::holdout)
        throw std::invalid_argument("distribution cannot explore holdout samples");
    const auto count = selection.end - selection.begin;
    const auto total = 2 * count;
    const auto advance = [&](std::size_t completed) {
        if (progress)
            progress(completed, total);
        if (stop.stop_requested())
            throw std::runtime_error("distribution analysis cancelled");
    };
    advance(0);
    Distribution result;
    result.rows.reserve(count);
    std::vector<double> values;
    values.reserve(count);
    for (auto i = selection.begin; i < selection.end; ++i) {
        const auto& original = source[i];
        DistributionRow row{i, {}, DistributionReason::other_role, {}};
        if (original.role == selection.role) {
            ++result.allowed;
            if (selection.metric == DistributionMetric::feature) {
                row.reason = feature_reason(original.feature.reason);
                if (row.reason == DistributionReason::valid)
                    row.value = original.feature.value.value();
            } else if (original.label_crosses_boundary) {
                row.reason = DistributionReason::crossed_label;
            } else if (!original.forward_return) {
                row.reason = DistributionReason::unknown_label;
            } else {
                row.reason = DistributionReason::valid;
                row.value = original.forward_return;
            }
            if (row.value && !std::isfinite(*row.value)) {
                row.value.reset();
                row.reason = DistributionReason::non_finite;
            }
        }
        if (row.value)
            values.push_back(*row.value);
        ++result.reasons[static_cast<std::size_t>(row.reason)];
        result.rows.push_back(std::move(row));
        if ((i - selection.begin + 1) % 4096 == 0)
            advance(i - selection.begin + 1);
    }
    result.valid = values.size();
    advance(count);
    if (values.empty()) {
        advance(total);
        return result;
    }
    std::sort(values.begin(), values.end());
    advance(count);
    result.minimum = values.front();
    result.maximum = values.back();
    const auto quantile = [&](double p) {
        const double position = static_cast<double>(values.size() - 1) * p;
        const auto lower = static_cast<std::size_t>(position);
        const auto upper = std::min(lower + 1, values.size() - 1);
        return std::lerp(values[lower], values[upper], position - static_cast<double>(lower));
    };
    result.q25 = quantile(.25);
    result.median = quantile(.5);
    result.q75 = quantile(.75);
    const long double scale = std::max(std::abs(values.front()), std::abs(values.back()));
    long double sum = 0, correction = 0;
    for (const auto value : values) {
        const auto normalized = scale == 0 ? 0.L : static_cast<long double>(value) / scale;
        const auto y = normalized - correction;
        const auto next = sum + y;
        correction = (next - sum) - y;
        sum = next;
    }
    const auto mean = sum / static_cast<long double>(values.size());
    const auto actual_mean = static_cast<double>(mean * scale);
    result.mean_state =
        std::isfinite(actual_mean) ? StatisticState::defined : StatisticState::non_finite;
    if (result.mean_state == StatisticState::defined)
        result.mean = actual_mean;
    if (values.size() == 1)
        result.standard_deviation_state = StatisticState::insufficient_samples;
    else {
        long double squares = 0;
        for (const auto value : values) {
            const auto delta = (scale == 0 ? 0.L : static_cast<long double>(value) / scale) - mean;
            squares += delta * delta;
        }
        const auto deviation = static_cast<double>(
            std::sqrt(squares / static_cast<long double>(values.size() - 1)) * scale);
        result.standard_deviation_state =
            std::isfinite(deviation) ? StatisticState::defined : StatisticState::non_finite;
        if (result.standard_deviation_state == StatisticState::defined)
            result.standard_deviation = deviation;
    }
    advance(count);
    const auto bins = values.front() == values.back() ? std::size_t{1} : selection.bins;
    std::vector<double> edges;
    for (std::size_t i = 0; i <= bins; ++i)
        edges.push_back(
            std::lerp(
                values.front(),
                values.back(),
                static_cast<double>(i) / static_cast<double>(bins)));
    for (std::size_t i = 0; i < bins; ++i)
        result.bins.push_back({edges[i], edges[i + 1], 0});
    for (std::size_t i = 0; i < result.rows.size(); ++i) {
        auto& row = result.rows[i];
        if (row.value) {
            const auto upper = std::upper_bound(edges.begin(), edges.end(), *row.value);
            const auto index =
                std::min(static_cast<std::size_t>(upper - edges.begin() - 1), bins - 1);
            row.bin = index;
            ++result.bins[index].count;
        }
        if ((i + 1) % 4096 == 0)
            advance(count + i + 1);
    }
    advance(total);
    return result;
}

GroupedDistribution describe_distribution_groups(
    std::span<const DistributionObservation> source,
    std::span<const std::string> trading_days,
    DistributionSelection selection,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress)
{
    if (trading_days.size() != source.size())
        throw std::invalid_argument("distribution trading days must match original observations");
    GroupedDistribution result;
    const auto advance = [&](std::size_t completed, std::size_t total) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (progress)
            progress(completed, total);
    };
    result.whole = describe_distribution(source, selection, stop, [&](auto done, auto total) {
        advance(done, 2 * total);
    });
    const auto count = selection.end - selection.begin;
    std::map<std::string, std::vector<std::size_t>> members;
    for (auto i = selection.begin; i < selection.end; ++i) {
        if (trading_days[i].empty())
            throw std::invalid_argument("distribution requires original trading days");
        members[trading_days[i]].push_back(i);
        if ((i - selection.begin) % 4096 == 0)
            advance(2 * count, 4 * count);
    }
    std::size_t completed = 2 * count;
    for (const auto& [key, indices] : members) {
        std::vector<DistributionObservation> observations;
        observations.reserve(indices.size());
        for (const auto i : indices)
            observations.push_back(source[i]);
        auto local = selection;
        local.begin = 0;
        local.end = observations.size();
        auto grouped = describe_distribution(observations, local, stop, [&](auto done, auto) {
            advance(completed + done, 4 * count);
        });
        grouped.bins = result.whole.bins;
        for (auto& bin : grouped.bins)
            bin.count = 0;
        for (std::size_t i = 0; i < grouped.rows.size(); ++i) {
            auto& row = grouped.rows[i];
            row.index = indices[i];
            row.bin = result.whole.rows[row.index - selection.begin].bin;
            if (row.bin)
                ++grouped.bins[*row.bin].count;
        }
        completed += 2 * observations.size();
        result.groups.push_back({key, std::move(grouped)});
    }
    advance(4 * count, 4 * count);
    return result;
}

} // namespace asterion
