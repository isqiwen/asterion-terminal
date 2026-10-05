#pragma once
#include <asterion/domain/live_market.hpp>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace asterion {
// One-minute bar aggregated from received quote observations. Volume is the
// difference of cumulative provider volume inside the minute, so the first
// observation of a trading day contributes no volume.
struct MinuteBar {
  std::int64_t start_ms = 0;
  Decimal open, high, low, close;
  std::int64_t volume = 0;
  std::optional<Decimal> average_price, open_interest;
};
struct IntradaySeries {
  InstrumentId instrument;
  std::string trading_day;
  // Where observation of this trading day began; earlier minutes are unknown.
  std::int64_t first_observation_ms = 0;
  // Observations were lost after first_observation_ms.
  bool interrupted = false;
  std::optional<Decimal> previous_settlement;
  std::vector<MinuteBar> bars;
};
// Builds the current trading day's minute bars and one-minute price change per
// instrument from ordered observations. It never invents observations; gaps
// are reported, not filled.
class IntradayBars {
public:
  void observe(const MarketQuote& quote);
  // Marks every series as incomplete after a lost or unordered observation.
  void interrupt();
  std::optional<IntradaySeries> series(const InstrumentId& instrument) const;
  // Percent change of last price versus the latest observation at least 60 s
  // older in the same trading day, rounded half-even to 8 places.
  std::optional<Decimal> change_1m_percent(const InstrumentId& instrument) const;
  // Independent of the provider snapshot cursor: aggregation can finish later.
  std::uint64_t revision() const { return revision_; }
  std::vector<InstrumentId> changed_after(std::uint64_t revision) const;

private:
  struct State {
    IntradaySeries series;
    std::int64_t cumulative_volume = 0;
    std::deque<std::pair<std::int64_t, Decimal>> recent;
    std::uint64_t revision = 0;
  };
  std::map<InstrumentId, State> states_;
  std::uint64_t revision_ = 0;
};
} // namespace asterion
