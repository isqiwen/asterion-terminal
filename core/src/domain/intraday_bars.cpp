#include <asterion/domain/intraday_bars.hpp>
#include <algorithm>
namespace asterion {
namespace {
constexpr std::int64_t minute_ms = 60000;
constexpr std::size_t max_bars = 24 * 60;
} // namespace
void IntradayBars::observe(const MarketQuote& quote) {
  if (!quote.last || quote.source_ms <= 0 || quote.trading_day.empty())
    return;
  auto& state = states_[quote.instrument];
  auto& series = state.series;
  if (series.trading_day != quote.trading_day) {
    // A new trading day starts a new series; the old one is not joined to it.
    state = {};
    series.instrument = quote.instrument;
    series.trading_day = quote.trading_day;
    series.first_observation_ms = quote.source_ms;
    state.cumulative_volume = quote.volume;
  }
  if (!series.bars.empty() && quote.source_ms < series.bars.back().start_ms)
    return;
  if (quote.previous_settlement)
    series.previous_settlement = quote.previous_settlement;
  // A falling cumulative counter is a provider reset, never negative volume.
  const auto delta = quote.volume >= state.cumulative_volume
                         ? quote.volume - state.cumulative_volume
                         : std::int64_t{0};
  if (quote.volume < state.cumulative_volume)
    series.interrupted = true;
  state.cumulative_volume = quote.volume;
  const auto start = quote.source_ms - quote.source_ms % minute_ms;
  const auto price = *quote.last;
  if (series.bars.empty() || series.bars.back().start_ms != start) {
    series.bars.push_back({start, price, price, price, price, 0, {}, {}});
    if (series.bars.size() > max_bars)
      series.bars.erase(series.bars.begin());
  }
  auto& bar = series.bars.back();
  bar.high = std::max(bar.high, price);
  bar.low = std::min(bar.low, price);
  bar.close = price;
  bar.volume += delta;
  if (quote.average_price)
    bar.average_price = quote.average_price;
  if (quote.open_interest)
    bar.open_interest = quote.open_interest;
  auto& recent = state.recent;
  if (!recent.empty() && recent.back().first == quote.source_ms)
    recent.back().second = price;
  else
    recent.emplace_back(quote.source_ms, price);
  // Keep only the newest observation that is already a minute old as anchor.
  while (recent.size() >= 2 && recent[1].first <= quote.source_ms - minute_ms)
    recent.pop_front();
}
void IntradayBars::interrupt() {
  for (auto& [id, state] : states_) {
    state.series.interrupted = true;
    state.recent.clear();
  }
}
std::optional<IntradaySeries> IntradayBars::series(const InstrumentId& instrument) const {
  const auto found = states_.find(instrument);
  if (found == states_.end())
    return std::nullopt;
  return found->second.series;
}
std::optional<Decimal> IntradayBars::change_1m_percent(const InstrumentId& instrument) const {
  const auto found = states_.find(instrument);
  if (found == states_.end() || found->second.recent.size() < 2)
    return std::nullopt;
  const auto& recent = found->second.recent;
  const auto& [anchor_ms, anchor] = recent.front();
  const auto& [latest_ms, latest] = recent.back();
  if (anchor_ms > latest_ms - minute_ms || anchor <= Decimal{})
    return std::nullopt;
  return divide(multiply(latest - anchor, Decimal::parse("100"), Rounding::half_even), anchor,
                Rounding::half_even);
}
} // namespace asterion
