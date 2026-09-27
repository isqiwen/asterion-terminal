#include "momentum.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>
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
MomentumFactor::MomentumFactor(Instrument instrument, std::size_t lookback)
    : instrument_(std::move(instrument)), lookback_(lookback) {
  instrument_.validate();
  if (!lookback || lookback > 10000)
    throw std::invalid_argument("lookback must be 1..10000 events");
}
PluginDescriptor MomentumFactor::descriptor() const {
  return {"asterion.tool.factor.momentum", PluginKind::tool, plugin_contract_version, {}};
}
void MomentumFactor::start() {
  if (running_)
    throw std::logic_error("factor already started");
  history_.clear();
  last_time_ = -1;
  running_ = true;
}
void MomentumFactor::stop() noexcept {
  running_ = false;
}
std::optional<double> MomentumFactor::on_tick(const TradeTick& tick) {
  if (!running_)
    throw std::logic_error("factor is stopped");
  tick.validate(instrument_);
  if (tick.price <= Decimal{} || tick.timestamp_ns < last_time_)
    throw std::invalid_argument("invalid factor price or event order");
  const auto result = history_.size() == lookback_
                          ? std::optional<double>(price_return(history_.front(), tick.price))
                          : std::nullopt;
  history_.push_back(tick.price);
  if (history_.size() > lookback_)
    history_.pop_front();
  last_time_ = tick.timestamp_ns;
  return result;
}
} // namespace asterion
