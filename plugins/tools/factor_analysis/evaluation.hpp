#pragma once
#include <asterion/domain/market.hpp>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>
namespace asterion {
double price_return(Decimal from, Decimal to);
// Undefined for constant series. Ranks use the average rank for ties.
std::optional<double> correlation(std::span<const double> x, std::span<const double> y);
std::optional<double> rank_correlation(std::span<const double> x, std::span<const double> y);
struct FactorCandidateScore {
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
using Feature = std::vector<std::optional<double>> (*)(std::span<const Decimal> values,
                                                       std::size_t lookback);
// Price momentum: close[i] / close[i - lookback] - 1, absent for the first
// `lookback` observations.
std::vector<std::optional<double>> momentum(std::span<const Decimal> closes, std::size_t lookback);
// The mean of the last `lookback` values, the observation's own included;
// absent until there are that many.
std::vector<std::optional<double>> average(std::span<const Decimal> values, std::size_t lookback);
// Label: close[i + horizon] / close[i] - 1, absent where the series ends
// first. Only the evaluation reads labels; they are never a feature's input.
std::vector<std::optional<double>> forward_returns(std::span<const Decimal> closes,
                                                   std::size_t horizon);

struct FactorSample {
  std::size_t index;
  double value, forward_return;
};
// Several series at one observation: how their features relate to their
// labels. Undefined where either is the same for all of them.
struct FactorCrossSection {
  std::size_t index;
  std::optional<double> pearson, spearman;
};
struct FactorPartition {
  std::size_t begin, end, samples;
  // One series: the correlation over its samples. Several: the mean of the
  // cross-sections that are defined.
  std::optional<double> pearson, spearman;
  // Several series: that mean over the standard deviation of the
  // cross-sections; undefined with fewer than two or when they do not vary.
  std::optional<double> pearson_ratio, spearman_ratio;
};
struct FactorCandidate {
  unsigned lookback;
  std::size_t samples;
  std::optional<double> development_spearman;
};
struct FactorEvaluation {
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
// `closes` are those of one contract each, at the same observations, and give
// the labels. `values` are what `feature` reads of each contract at those
// observations: the closes again for momentum. `lookbacks` ascend; every
// window is evaluated on the same observations, after the largest one has
// warmed up. With several windows `split` is required, and the choice among
// them is fixed from observations before it: features and labels of that
// stage are computed from the first `split` observations alone. `progress`
// receives completed and total work: one unit per observation, and per
// development observation for each compared window.
FactorEvaluation
evaluate_factor(std::span<const std::span<const Decimal>> closes,
                std::span<const std::span<const Decimal>> values, Feature feature,
                std::span<const unsigned> lookbacks, std::size_t horizon,
                std::optional<std::size_t> split, std::stop_token stop = {},
                const std::function<void(std::size_t, std::size_t)>& progress = {});
} // namespace asterion
