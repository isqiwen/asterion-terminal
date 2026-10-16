#pragma once
#include <asterion/domain/market.hpp>
#include <functional>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace asterion {

double price_return(Decimal from, Decimal to);
// Undefined for constant series. Ranks use the average rank for ties.
std::optional<double> correlation(std::span<const double> x, std::span<const double> y);
std::optional<double> rank_correlation(std::span<const double> x, std::span<const double> y);

struct FactorCandidateScore
{
    unsigned lookback;
    std::optional<double> development_spearman;
};

// Fixed policy: maximum absolute development rank correlation; exact ties
// choose the smaller window. Undefined scores are excluded, never zero-filled.
std::optional<unsigned> select_lookback(std::span<const FactorCandidateScore> candidates);

// The evaluation of a factor runs in stages over ordered observations; who
// made them and when is the caller's business. One series is judged by how
// its feature relates to its label over time, several by how features relate
// to labels across the series at each observation.

// A feature is computed from one series of values with a window. An entry
// reads nothing after its own observation.
using Feature = std::vector<std::optional<double>> (*)(
    std::span<const Decimal> values,
    std::size_t lookback);
// Price momentum: close[i] / close[i - lookback] - 1, absent for the first
// `lookback` observations.
std::vector<std::optional<double>> momentum(std::span<const Decimal> closes, std::size_t lookback);
// The mean of the last `lookback` values, the observation's own included;
// absent until there are that many.
std::vector<std::optional<double>> average(std::span<const Decimal> values, std::size_t lookback);
// Volatility: the sample standard deviation of the last `lookback` returns,
// each from one close to the next and the observation's own included; absent
// for the first `lookback` observations. Closes that did not move give zero.
std::vector<std::optional<double>>
volatility(std::span<const Decimal> closes, std::size_t lookback);
// Label: close[i + horizon] / close[i] - 1, absent where the series ends
// first. Only the evaluation reads labels; they are never a feature's input.
std::vector<std::optional<double>>
forward_returns(std::span<const Decimal> closes, std::size_t horizon);

struct FactorSample
{
    std::size_t index;
    double value, forward_return;
};

struct FactorMember
{
    std::size_t series_index;
    double value, forward_return;
};

// Several series at one observation: how the features of those that have one
// there relate to their labels. Undefined with
// fewer than three of them, and where either is the same for all of them.
struct FactorCrossSection
{
    std::size_t index;
    std::optional<double> pearson, spearman;
    // Highest feature first; equal features keep input order. This is the
    // grouping order, not the average ranks used by rank correlation.
    std::vector<FactorMember> members;
};

// The mean label of the series with the highest features and of those with
// the lowest, a number of them on each side, over the `observations` that had
// enough series for two such sides; zeros when none had.
struct FactorSpread
{
    double highest, lowest;
    std::size_t observations = 0;
};

struct FactorPartition
{
    std::size_t begin, end, samples;
    // One series: the correlation over its samples. Several: the mean of the
    // cross-sections that are defined.
    std::optional<double> pearson, spearman;
    // Several series: that mean over the standard deviation of the
    // cross-sections; undefined with fewer than two or when they do not vary.
    std::optional<double> pearson_ratio, spearman_ratio;
    // Several series: entry k - 1 holds k of them on each side, up to half of
    // them, averaged over the observations. Equal features keep the order the
    // series were given in, as a ranking rule holds them.
    std::vector<FactorSpread> spreads;
};

struct FactorCandidate
{
    unsigned lookback;
    std::size_t samples;
    std::optional<double> development_spearman;
};

struct FactorEvaluation
{
    // The window the rows were computed with: the only one given, or the one
    // the development observations selected.
    unsigned lookback;
    // One entry per window when several were given; development evidence only.
    std::vector<FactorCandidate> candidates;
    // The whole series, or development then holdout when `split` is given.
    std::vector<FactorPartition> partitions;
    // One row per evaluated observation: samples of one series, cross-sections
    // of several. The other list is empty.
    std::vector<FactorSample> samples;
    std::vector<FactorCrossSection> cross_sections;
    // Development observations whose label would reach into the holdout.
    std::size_t purged = 0;
};

// What one series has at each observation of an evaluation; nothing where it
// was not observed.
using FactorValues = std::vector<std::optional<Decimal>>;
// `closes` are those of one contract each over the same observations, and give
// the labels. `values` are what `feature` reads of each contract at those
// observations: the closes again for momentum. A series need not have every
// observation: its feature and its label are computed over observations it
// has without a break (a window or a label never reaches across one it
// lacks), and an observation compares the series that have both there. One
// series alone has all of its observations. `lookbacks` ascend; every
// window is evaluated on the same observations, after the largest one has
// warmed up. With several windows `split` is required, and the choice among
// them is fixed from observations before it: features and labels of that
// stage are computed from the first `split` observations alone. `progress`
// receives completed and total work: one unit per observation, and per
// development observation for each compared window.
FactorEvaluation evaluate_factor(
    std::span<const std::span<const std::optional<Decimal>>> closes,
    std::span<const std::span<const std::optional<Decimal>>> values,
    Feature feature,
    std::span<const unsigned> lookbacks,
    std::size_t horizon,
    std::optional<std::size_t> split,
    std::stop_token stop = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {},
    std::span<const std::span<const std::uint8_t>> eligibility = {});

} // namespace asterion
