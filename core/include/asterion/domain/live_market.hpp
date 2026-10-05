#pragma once
#include <asterion/domain/market.hpp>
#include <optional>
#include <span>
#include <array>
#include <variant>
#include <vector>
namespace asterion {
struct MarketDepthLevel {
  std::optional<Decimal> price;
  std::optional<std::int64_t> quantity;
};
// Market observations are not authoritative account ledger entries.
struct MarketQuote {
  InstrumentId instrument;
  // Additional levels 2..5; best bid/ask remain the quote summary above the book.
  std::array<MarketDepthLevel, 4> bid_levels, ask_levels;
  std::optional<Decimal> last, bid, ask, previous_settlement, high, low, open_interest;
  std::optional<Decimal> open, upper_limit, lower_limit, previous_close;
  // Current open interest minus the provider previous-trading-day reference.
  std::optional<Decimal> open_interest_change;
  // Per-unit session average price; absent when the provider cannot normalize it.
  std::optional<Decimal> average_price;
  std::int64_t bid_quantity = 0, ask_quantity = 0, volume = 0;
  std::string action_day, trading_day, update_time;
  std::int64_t source_ms = 0, received_ms = 0;
};
struct MarketSubscription {
  InstrumentId instrument;
  std::string state = "pending";
  int error_code = 0;
  std::optional<MarketQuote> quote;
};
struct LiveMarketSnapshot {
  std::string phase = "disconnected";
  int error_code = 0;
  std::uint64_t sequence = 0, out_of_order = 0;
  // A delta preserves subscription membership and includes only changed rows.
  bool subscriptions_delta = false;
  std::vector<MarketSubscription> subscriptions;
};
// Ordered, normalized provider observations, distinct from coalesced UI state.
// Session status events carry subscriptions without cached quotes. Individual
// subscription acknowledgements carry only their changed subscription, also
// without a quote. Volume is cumulative provider volume, not trade quantity.
struct MarketQuoteObservation {
  MarketQuote quote;
  bool out_of_order = false;
};
struct MarketEvent {
  std::uint64_t sequence = 0;
  std::int64_t received_ms = 0;
  std::variant<LiveMarketSnapshot, MarketQuoteObservation, MarketSubscription> value;
};
struct MarketEventBatch {
  std::string stream_id;
  std::uint64_t oldest_sequence = 0, latest_sequence = 0;
  // A gap means records after the requested cursor have been evicted. A failed
  // stream is permanently incomplete even if its retained prefix is readable.
  bool gap = false, failed = false;
  std::vector<MarketEvent> events;
};
// Provider-neutral live market port; credentials/configuration belong to the
// provider.
class LiveMarketDataPort {
public:
  virtual ~LiveMarketDataPort() = default;
  virtual void subscribe(const std::vector<InstrumentId>& instruments) = 0;
  virtual LiveMarketSnapshot snapshot(std::optional<std::uint64_t> after = {},
                                      std::span<const InstrumentId> forced = {}) const = 0;
  // Non-destructive bounded reads. Only the initial cursor (0) may omit stream
  // identity. Consumers advance their cursor only after committing the batch.
  virtual MarketEventBatch events_after(const std::string& stream_id, std::uint64_t cursor,
                                        std::size_t limit) const = 0;
  virtual void disconnect() = 0;
};
} // namespace asterion
