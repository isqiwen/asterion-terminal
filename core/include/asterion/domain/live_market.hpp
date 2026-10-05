#pragma once
#include <asterion/domain/market.hpp>
#include <optional>
#include <span>
#include <string_view>
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
enum class SubscriptionState { pending, subscribed, error };
constexpr std::string_view subscription_state_name(SubscriptionState state) noexcept {
  switch (state) {
  case SubscriptionState::pending:
    return "pending";
  case SubscriptionState::subscribed:
    return "subscribed";
  case SubscriptionState::error:
    return "error";
  }
  return "error";
}
enum class MarketPhase { disconnected, connecting, logging_in, reconnecting, connected, error };
constexpr std::string_view market_phase_name(MarketPhase phase) noexcept {
  switch (phase) {
  case MarketPhase::disconnected:
    return "disconnected";
  case MarketPhase::connecting:
    return "connecting";
  case MarketPhase::logging_in:
    return "logging_in";
  case MarketPhase::reconnecting:
    return "reconnecting";
  case MarketPhase::connected:
    return "connected";
  case MarketPhase::error:
    return "error";
  }
  return "error";
}
struct MarketSubscription {
  InstrumentId instrument;
  SubscriptionState state = SubscriptionState::pending;
  int error_code = 0;
  std::optional<MarketQuote> quote;
};
struct LiveMarketSnapshot {
  MarketPhase phase = MarketPhase::disconnected;
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
} // namespace asterion
