#include "momentum.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>
#include <stdexcept>
namespace asterion {
std::optional<unsigned>
select_momentum_lookback(std::span<const MomentumCandidateScore> candidates) {
  if (candidates.empty() || candidates.size() > 32)
    throw std::invalid_argument("momentum selection needs 1..32 candidates");
  unsigned previous = 0;
  std::optional<unsigned> selected;
  double best = -1;
  for (const auto& candidate : candidates) {
    if (candidate.lookback <= previous || candidate.lookback > 10000)
      throw std::invalid_argument("momentum windows must be ascending and unique");
    previous = candidate.lookback;
    if (!candidate.development_spearman)
      continue;
    const auto score = *candidate.development_spearman;
    if (!std::isfinite(score) || score < -1 || score > 1)
      throw std::invalid_argument("invalid development score");
    if (std::abs(score) > best) {
      best = std::abs(score);
      selected = candidate.lookback;
    }
  }
  return selected;
}
double price_return(Decimal from, Decimal to) {
  if (from <= Decimal{} || to <= Decimal{})
    throw std::invalid_argument("return requires positive prices");
  // Both units are positive: subtraction cannot overflow. Subtract before
  // floating conversion to retain small price changes at large magnitudes.
  return static_cast<double>(static_cast<long double>(to.raw() - from.raw()) /
                             static_cast<long double>(from.raw()));
}
std::optional<double> correlation(std::span<const double> x, std::span<const double> y) {
  if (x.size() != y.size() || x.size() < 2)
    throw std::invalid_argument("correlation requires equally sized series "
                                "with at least two observations");
  long double mx = 0, my = 0, sx = 0, sy = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    if (!std::isfinite(x[i]) || !std::isfinite(y[i]))
      throw std::invalid_argument("non-finite observation");
    sx = std::max(sx, std::fabs(static_cast<long double>(x[i])));
    sy = std::max(sy, std::fabs(static_cast<long double>(y[i])));
  }
  if (sx == 0 || sy == 0)
    return std::nullopt;
  // Scale before centering; this also works where long double is only 64 bits.
  for (std::size_t i = 0; i < x.size(); ++i) {
    mx += static_cast<long double>(x[i]) / sx;
    my += static_cast<long double>(y[i]) / sy;
  }
  mx /= static_cast<long double>(x.size());
  my /= static_cast<long double>(x.size());
  long double xx = 0, yy = 0, xy = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const auto dx = static_cast<long double>(x[i]) / sx - mx,
               dy = static_cast<long double>(y[i]) / sy - my;
    xx += dx * dx;
    yy += dy * dy;
    xy += dx * dy;
  }
  if (xx == 0 || yy == 0)
    return std::nullopt;
  return static_cast<double>(std::clamp(xy / std::sqrt(xx * yy), -1.0L, 1.0L));
}
namespace {
std::vector<double> ranks(std::span<const double> values) {
  std::vector<std::size_t> order(values.size());
  std::iota(order.begin(), order.end(), 0);
  for (auto value : values)
    if (!std::isfinite(value))
      throw std::invalid_argument("non-finite observation");
  std::sort(order.begin(), order.end(), [&](auto a, auto b) { return values[a] < values[b]; });
  std::vector<double> result(values.size());
  for (std::size_t begin = 0; begin < order.size();) {
    auto end = begin + 1;
    while (end < order.size() && values[order[end]] == values[order[begin]])
      ++end;
    const auto rank = (static_cast<double>(begin) + static_cast<double>(end - 1)) / 2 + 1;
    for (auto i = begin; i < end; ++i)
      result[order[i]] = rank;
    begin = end;
  }
  return result;
}
} // namespace
std::optional<double> rank_correlation(std::span<const double> x, std::span<const double> y) {
  return correlation(ranks(x), ranks(y));
}
std::vector<std::optional<double>> momentum(std::span<const Decimal> closes, std::size_t lookback) {
  if (!lookback || lookback > 10000)
    throw std::invalid_argument("momentum lookback must be 1..10000 observations");
  std::vector<std::optional<double>> result(closes.size());
  for (std::size_t i = lookback; i < closes.size(); ++i)
    result[i] = price_return(closes[i - lookback], closes[i]);
  return result;
}
std::vector<std::optional<double>> forward_returns(std::span<const Decimal> closes,
                                                   std::size_t horizon) {
  if (!horizon || horizon > 10000)
    throw std::invalid_argument("forward return horizon must be 1..10000 observations");
  std::vector<std::optional<double>> result(closes.size());
  for (std::size_t i = 0; i + horizon < closes.size(); ++i)
    result[i] = price_return(closes[i], closes[i + horizon]);
  return result;
}
MomentumEvaluation
evaluate_momentum(std::span<const Decimal> closes, std::span<const unsigned> lookbacks,
                  std::size_t horizon, std::optional<std::size_t> split, std::stop_token stop,
                  const std::function<void(std::size_t, std::size_t)>& progress) {
  const auto count = closes.size();
  if (lookbacks.empty() || (lookbacks.size() > 1 && !split) || (split && *split >= count))
    throw std::invalid_argument("momentum evaluation needs windows and, to compare them, a "
                                "boundary inside the series");
  for (const auto close : closes)
    if (close <= Decimal{})
      throw std::invalid_argument("return requires positive prices");
  const std::size_t warmup = lookbacks.back();
  const auto boundary = split.value_or(count);
  const auto total = count + (lookbacks.size() > 1 ? lookbacks.size() * boundary : 0);
  std::size_t completed = 0;
  const auto advance = [&](std::size_t units) {
    completed += units;
    if (progress)
      progress(completed, total);
    if (stop.stop_requested())
      throw std::runtime_error("factor analysis cancelled");
  };
  advance(0);
  MomentumEvaluation result;
  result.lookback = lookbacks.front();
  if (lookbacks.size() > 1) {
    // Selection sees the development observations and nothing else.
    const auto development = closes.first(boundary);
    const auto labels = forward_returns(development, horizon);
    std::vector<MomentumCandidateScore> scores;
    for (const auto window : lookbacks) {
      const auto features = momentum(development, window);
      std::vector<double> values, outcomes;
      for (auto i = warmup; i < boundary; ++i)
        if (features[i] && labels[i]) {
          values.push_back(*features[i]);
          outcomes.push_back(*labels[i]);
        }
      const auto score = rank_correlation(values, outcomes);
      result.candidates.push_back({window, values.size(), score});
      scores.push_back({window, score});
      advance(boundary);
    }
    const auto selection = select_momentum_lookback(scores);
    if (!selection)
      throw std::invalid_argument("no candidate has a defined development correlation");
    result.lookback = *selection;
  }
  const auto features = momentum(closes, result.lookback);
  const auto labels = forward_returns(closes, horizon);
  std::vector<double> values[2], outcomes[2];
  for (auto i = warmup; i < count; ++i) {
    if (!features[i] || !labels[i])
      continue;
    // A development label that ends in the holdout would let the two overlap.
    if (i < boundary && i + horizon >= boundary) {
      ++result.purged;
      continue;
    }
    const auto partition = i < boundary ? 0 : 1;
    result.samples.push_back({i, *features[i], *labels[i]});
    values[partition].push_back(*features[i]);
    outcomes[partition].push_back(*labels[i]);
  }
  for (std::size_t index = 0; index < (split ? 2U : 1U); ++index)
    result.partitions.push_back({index ? boundary : 0, index || !split ? count : boundary,
                                 values[index].size(), correlation(values[index], outcomes[index]),
                                 rank_correlation(values[index], outcomes[index])});
  advance(count);
  return result;
}
} // namespace asterion
