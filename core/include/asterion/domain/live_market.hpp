#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/kernel/plugin.hpp>
#include <optional>
#include <variant>
#include <vector>
namespace asterion {
// Market observations are not authoritative account ledger entries.
struct MarketQuote {
  InstrumentId instrument;
  std::optional<Decimal> last, bid, ask, previous_settlement, high, low, open_interest;
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
  std::vector<MarketSubscription> subscriptions;
};
// Ordered, normalized provider observations, distinct from coalesced UI state.
// Status events carry subscriptions without cached quotes. Volume is cumulative
// provider volume, not the quantity of an individual trade.
struct MarketQuoteObservation {
  MarketQuote quote;
  bool out_of_order = false;
};
struct MarketEvent {
  std::uint64_t sequence = 0;
  std::int64_t received_ms = 0;
  std::variant<LiveMarketSnapshot, MarketQuoteObservation> value;
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
class LiveMarketDataPort : public Plugin {
public:
  virtual ~LiveMarketDataPort() = default;
  virtual void subscribe(const std::vector<InstrumentId>& instruments) = 0;
  virtual LiveMarketSnapshot snapshot() const = 0;
  // Non-destructive bounded reads. Only the initial cursor (0) may omit stream
  // identity. Consumers advance their cursor only after committing the batch.
  virtual MarketEventBatch events_after(const std::string& stream_id, std::uint64_t cursor,
                                        std::size_t limit) const = 0;
  virtual void disconnect() = 0;
};
} // namespace asterion
