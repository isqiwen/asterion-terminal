#include "evaluation.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>
#include <stdexcept>
namespace asterion {
std::optional<unsigned> select_lookback(std::span<const FactorCandidateScore> candidates) {
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
std::vector<std::optional<double>> average(std::span<const Decimal> values, std::size_t lookback) {
  std::vector<std::optional<double>> result(values.size());
  long double sum = 0;
  for (std::size_t i = 0; i < values.size(); ++i) {
    sum += static_cast<long double>(values[i].raw());
    if (i >= lookback)
      sum -= static_cast<long double>(values[i - lookback].raw());
    // Raw decimals carry eight places.
    if (lookback && i + 1 >= lookback)
      result[i] = static_cast<double>(sum / static_cast<long double>(lookback) / 100000000);
  }
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
namespace {
using Column = std::vector<std::optional<double>>;
struct Statistics {
  std::optional<double> pearson, spearman, pearson_ratio, spearman_ratio;
};
// The mean of the defined values, and that mean over their standard deviation.
std::pair<std::optional<double>, std::optional<double>>
summary(std::span<const FactorCrossSection> sections,
        std::optional<double> FactorCrossSection::* value) {
  long double sum = 0, squares = 0;
  std::size_t count = 0;
  for (const auto& section : sections)
    if (section.*value) {
      sum += *(section.*value);
      ++count;
    }
  if (!count)
    return {};
  const auto mean = sum / static_cast<long double>(count);
  for (const auto& section : sections)
    if (section.*value)
      squares += (*(section.*value) - mean) * (*(section.*value) - mean);
  std::optional<double> ratio;
  if (count > 1 && squares > 0)
    ratio = static_cast<double>(mean / std::sqrt(squares / static_cast<long double>(count - 1)));
  return {static_cast<double>(mean), ratio};
}
Statistics statistics(std::span<const FactorCrossSection> sections) {
  const auto [pearson, pearson_ratio] = summary(sections, &FactorCrossSection::pearson);
  const auto [spearman, spearman_ratio] = summary(sections, &FactorCrossSection::spearman);
  return {pearson, spearman, pearson_ratio, spearman_ratio};
}
// Rows where every series has its feature and its label; the series share
// their observations, so one of them decides.
std::vector<std::size_t> evaluated(const Column& feature, const Column& label, std::size_t begin,
                                   std::size_t end) {
  std::vector<std::size_t> rows;
  for (auto i = begin; i < end; ++i)
    if (feature[i] && label[i])
      rows.push_back(i);
  return rows;
}
std::pair<std::vector<double>, std::vector<double>>
over_time(const Column& feature, const Column& label, std::span<const std::size_t> rows) {
  std::vector<double> values, outcomes;
  for (const auto row : rows) {
    values.push_back(*feature[row]);
    outcomes.push_back(*label[row]);
  }
  return {std::move(values), std::move(outcomes)};
}
std::vector<FactorCrossSection> across_series(std::span<const Column> features,
                                              std::span<const Column> labels,
                                              std::span<const std::size_t> rows) {
  std::vector<FactorCrossSection> sections;
  std::vector<double> values(features.size()), outcomes(features.size());
  for (const auto row : rows) {
    for (std::size_t s = 0; s < features.size(); ++s) {
      values[s] = *features[s][row];
      outcomes[s] = *labels[s][row];
    }
    sections.push_back({row, correlation(values, outcomes), rank_correlation(values, outcomes)});
  }
  return sections;
}
} // namespace
FactorEvaluation evaluate_factor(std::span<const std::span<const Decimal>> series,
                                 std::span<const std::span<const Decimal>> values, Feature feature,
                                 std::span<const unsigned> lookbacks, std::size_t horizon,
                                 std::optional<std::size_t> split, std::stop_token stop,
                                 const std::function<void(std::size_t, std::size_t)>& progress) {
  if (series.empty() || values.size() != series.size())
    throw std::invalid_argument("factor evaluation needs a series and its values");
  const auto count = series.front().size();
  if (lookbacks.empty() || (lookbacks.size() > 1 && !split) || (split && *split >= count))
    throw std::invalid_argument("factor evaluation needs windows and, to compare them, a "
                                "boundary inside the series");
  for (std::size_t s = 0; s < series.size(); ++s) {
    if (series[s].size() != count || values[s].size() != count)
      throw std::invalid_argument("factor series must share their observations");
    for (const auto close : series[s])
      if (close <= Decimal{})
        throw std::invalid_argument("return requires positive prices");
  }
  const bool cross = series.size() > 1;
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
  // Labels of the closes and features of the values, from the first `end`
  // observations alone.
  const auto labelled = [&](std::size_t end) {
    std::vector<Column> result;
    for (const auto closes : series)
      result.push_back(forward_returns(closes.first(end), horizon));
    return result;
  };
  const auto featured = [&](std::size_t end, std::size_t window) {
    std::vector<Column> result;
    for (const auto input : values)
      result.push_back(feature(input.first(end), window));
    return result;
  };
  FactorEvaluation result;
  result.lookback = lookbacks.front();
  if (lookbacks.size() > 1) {
    // Selection sees the development observations and nothing else.
    const auto labels = labelled(boundary);
    std::vector<FactorCandidateScore> scores;
    for (const auto window : lookbacks) {
      const auto features = featured(boundary, window);
      const auto rows = evaluated(features.front(), labels.front(), warmup, boundary);
      std::optional<double> score;
      if (cross) {
        score = statistics(across_series(features, labels, rows)).spearman;
      } else {
        const auto [values, outcomes] = over_time(features.front(), labels.front(), rows);
        score = rank_correlation(values, outcomes);
      }
      result.candidates.push_back({window, rows.size(), score});
      scores.push_back({window, score});
      advance(boundary);
    }
    const auto selection = select_lookback(scores);
    if (!selection)
      throw std::invalid_argument("no candidate has a defined development correlation");
    result.lookback = *selection;
  }
  const auto features = featured(count, result.lookback);
  const auto labels = labelled(count);
  for (std::size_t index = 0; index < (split ? 2U : 1U); ++index) {
    const auto begin = index ? boundary : 0, end = index || !split ? count : boundary;
    auto rows = evaluated(features.front(), labels.front(), std::max(begin, warmup), end);
    if (end < count) {
      // A development label that ends in the holdout would let the two overlap.
      const auto crossing =
          std::ranges::remove_if(rows, [&](auto row) { return row + horizon >= end; });
      result.purged += crossing.size();
      rows.erase(crossing.begin(), crossing.end());
    }
    if (cross) {
      const auto sections = across_series(features, labels, rows);
      const auto summary = statistics(sections);
      result.partitions.push_back({begin, end, rows.size(), summary.pearson, summary.spearman,
                                   summary.pearson_ratio, summary.spearman_ratio});
      result.cross_sections.insert(result.cross_sections.end(), sections.begin(), sections.end());
    } else {
      const auto [values, outcomes] = over_time(features.front(), labels.front(), rows);
      result.partitions.push_back({begin,
                                   end,
                                   rows.size(),
                                   correlation(values, outcomes),
                                   rank_correlation(values, outcomes),
                                   {},
                                   {}});
      for (std::size_t i = 0; i < rows.size(); ++i)
        result.samples.push_back({rows[i], values[i], outcomes[i]});
    }
  }
  advance(count);
  return result;
}
} // namespace asterion
