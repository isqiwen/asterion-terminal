#pragma once
#include <asterion/domain/broker_send_gate.hpp>
#include <asterion/domain/account.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
namespace asterion {
// Asterion's own outcomes share error_code and dispatch result fields with the
// provider's codes, which never use this range.
namespace broker_code {
inline constexpr int sdk_failure = -1000;
inline constexpr int login_incomplete = -1001;
inline constexpr int query_timeout = -1002;
// The connection that prepared the request has ended or is closing.
inline constexpr int session_changed = -1003;
inline constexpr int exposure_changed = -1004;
inline constexpr int expired = -1005;
inline constexpr int dispatch_busy = -1006;
inline constexpr int permit_refused = -1007;
// Accepted events were kept; the account needs reconciliation.
inline constexpr int callback_overflow = -1008;
} // namespace broker_code
// Live accounts are owned by the broker. These are broker-reported
// observations converted at the provider boundary, not a local ledger; money
// arrives as provider values rounded to 0.01.
enum class BrokerOrderStatus { submitted, accepted, partially_filled, filled, cancelled, rejected };
struct BrokerOrder {
  // Caller identity; empty for an order this process cannot attribute (placed
  // by another client, or a journal that did not record the broker key).
  std::string order_id;
  // Provider identity allocated before sending, e.g. CTP "front:session:ref".
  std::string broker_key;
  // Exchange identity once accepted, e.g. "SHFE:      123456" trimmed.
  std::string exchange_order_id;
  InstrumentId instrument;
  Side side = Side::buy;
  Offset offset = Offset::open;
  Decimal quantity, filled, limit_price;
  BrokerOrderStatus status = BrokerOrderStatus::submitted;
  int error_code = 0;
};
struct BrokerTrade {
  std::string trade_id; // "<venue>:<exchange trade id>", unique per trading day
  std::string order_id, exchange_order_id;
  InstrumentId instrument;
  Side side = Side::buy;
  Offset offset = Offset::open;
  Decimal quantity, price;
  std::string trading_day, trade_time;
};
struct BrokerPosition {
  InstrumentId instrument;
  Side side = Side::buy;
  Decimal today, yesterday;
};
struct BrokerFunds {
  Decimal balance, available, margin, commission, close_profit, position_profit;
};
// Margin and commission rates the broker applies to this account for one
// contract. Margin takes the higher of the long and short rates. "ready"
// once both queries answered with a row; "unavailable" if either returned
// none or failed.
enum class BrokerCostsState { querying, ready, unavailable };
constexpr std::string_view broker_costs_state_name(BrokerCostsState state) noexcept {
  switch (state) {
  case BrokerCostsState::querying:
    return "querying";
  case BrokerCostsState::ready:
    return "ready";
  case BrokerCostsState::unavailable:
    return "unavailable";
  }
  return "unavailable";
}
struct BrokerCosts {
  InstrumentId instrument;
  BrokerCostsState state = BrokerCostsState::querying;
  int error_code = 0;
  std::int64_t queried_ms = 0;
  std::optional<FuturesCosts> costs;
};
// The broker's current market for one contract; prices the exchange has not
// set (no trade yet, no limit) are absent.
struct BrokerQuote {
  InstrumentId instrument;
  std::optional<Decimal> last, pre_settlement, upper_limit, lower_limit;
  std::string trading_day, update_time;
  std::uint64_t connection_generation = 0;
  // Successful query completion on the account process's monotonic clock.
  // Distinct from exchange trade time; never serialized as a remote timestamp.
  std::chrono::steady_clock::time_point completed_at;
};
enum class BrokerPhase {
  disconnected,
  connecting,
  authenticating,
  logging_in,
  confirming,
  synchronizing,
  ready,
  error
};
constexpr std::string_view broker_phase_name(BrokerPhase phase) noexcept {
  switch (phase) {
  case BrokerPhase::disconnected:
    return "disconnected";
  case BrokerPhase::connecting:
    return "connecting";
  case BrokerPhase::authenticating:
    return "authenticating";
  case BrokerPhase::logging_in:
    return "logging_in";
  case BrokerPhase::confirming:
    return "confirming";
  case BrokerPhase::synchronizing:
    return "synchronizing";
  case BrokerPhase::ready:
    return "ready";
  case BrokerPhase::error:
    return "error";
  }
  return "error";
}
struct BrokerSnapshot {
  // Orders are accepted only when ready.
  BrokerPhase phase = BrokerPhase::disconnected;
  int error_code = 0;
  std::string trading_day;
  // Increments on every change; lets callers skip unchanged snapshots.
  std::uint64_t sequence = 0;
  // Changes whenever a connection ends; authorization is scoped to this value.
  std::uint64_t connection_generation = 0;
  // Risk must use this revision when submitting. Opening orders require all
  // observed fills to be included in a completed position query.
  std::uint64_t exposure_revision = 0;
  bool positions_reconciled = false;
  // Wall-clock ms of the last completed funds/positions/orders/trades query.
  std::int64_t synchronized_ms = 0;
  std::optional<BrokerFunds> funds;
  std::vector<BrokerPosition> positions;
  std::vector<BrokerOrder> orders;
  std::vector<BrokerTrade> trades;
  std::vector<BrokerCosts> costs;
};
// A provider-prepared order owns immutable identity and wire data. Preparation
// has no broker side effects. Destroying it without dispatch sends nothing.
// The originating trader must outlive the preparation.
class PreparedBrokerOrder {
public:
  virtual ~PreparedBrokerOrder() = default;
  PreparedBrokerOrder(const PreparedBrokerOrder&) = delete;
  PreparedBrokerOrder& operator=(const PreparedBrokerOrder&) = delete;
  const BrokerOrder& order() const noexcept { return order_; }

protected:
  explicit PreparedBrokerOrder(BrokerOrder order) : order_(std::move(order)) {}

private:
  const BrokerOrder order_;
};
struct BrokerDispatchResult {
  int code = 0;
  // False is positive evidence that the SDK request was never invoked.
  // True does not prove acceptance, execution, rejection or cancellation.
  bool invoked = false;
};
} // namespace asterion
