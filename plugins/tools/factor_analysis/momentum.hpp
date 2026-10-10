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
struct MomentumCandidateScore {
  unsigned lookback;
  std::optional<double> development_spearman;
};
// Fixed policy: maximum absolute development rank correlation; exact ties
// choose the smaller window. Undefined scores are excluded, never zero-filled.
std::optional<unsigned>
select_momentum_lookback(std::span<const MomentumCandidateScore> candidates);

// The evaluation of a momentum factor runs in stages over ordered, positive
// closes; who made the observations and when is the caller's business.

// Feature: close[i] / close[i - lookback] - 1, absent for the first `lookback`
// observations. An entry reads nothing after its own observation.
std::vector<std::optional<double>> momentum(std::span<const Decimal> closes, std::size_t lookback);
// Label: close[i + horizon] / close[i] - 1, absent where the series ends
// first. Only the evaluation reads labels; they are never a feature's input.
std::vector<std::optional<double>> forward_returns(std::span<const Decimal> closes,
                                                   std::size_t horizon);

struct MomentumSample {
  std::size_t index;
  double value, forward_return;
};
struct MomentumPartition {
  std::size_t begin, end, samples;
  std::optional<double> pearson, spearman;
};
struct MomentumCandidate {
  unsigned lookback;
  std::size_t samples;
  std::optional<double> development_spearman;
};
struct MomentumEvaluation {
  // The window the samples were computed with: the only one given, or the one
  // the development observations selected.
  unsigned lookback;
  // One entry per window when several were given; development evidence only.
  std::vector<MomentumCandidate> candidates;
  // The whole series, or development then holdout when `split` is given.
  std::vector<MomentumPartition> partitions;
  std::vector<MomentumSample> samples;
  // Development samples whose label would reach into the holdout.
  std::size_t purged = 0;
};
// `lookbacks` ascend; every window is evaluated on the same samples, after the
// largest one has warmed up. With several windows `split` is required, and the
// choice among them is fixed from observations before it: features and labels
// of that stage are computed from closes[0, split) alone. `progress` receives
// completed and total work: one unit per observation, and per development
// observation for each compared window.
MomentumEvaluation
evaluate_momentum(std::span<const Decimal> closes, std::span<const unsigned> lookbacks,
                  std::size_t horizon, std::optional<std::size_t> split, std::stop_token stop = {},
                  const std::function<void(std::size_t, std::size_t)>& progress = {});
} // namespace asterion
