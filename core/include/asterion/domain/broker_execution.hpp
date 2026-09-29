#pragma once
#include <asterion/domain/account.hpp>
#include <asterion/kernel/plugin.hpp>
#include <functional>
#include <optional>
#include <vector>
namespace asterion {
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
struct BrokerSnapshot {
  // disconnected, connecting, authenticating, logging_in, confirming,
  // synchronizing, ready, error. Orders are accepted only when ready.
  std::string phase = "disconnected";
  int error_code = 0;
  std::string trading_day;
  // Increments on every change; lets callers skip unchanged snapshots.
  std::uint64_t sequence = 0;
  // Wall-clock ms of the last completed funds/positions/orders/trades query.
  std::int64_t synchronized_ms = 0;
  std::optional<BrokerFunds> funds;
  std::vector<BrokerPosition> positions;
  std::vector<BrokerOrder> orders;
  std::vector<BrokerTrade> trades;
};
// Provider-neutral live execution. Credentials and configuration belong to
// the provider. Pre-trade risk, authorization and journaling are the caller's
// duty; the provider only routes and reports.
class BrokerExecutionPort : public Plugin {
public:
  // Allocates the broker key and calls `journal` with the pending order before
  // anything is sent; the caller makes that record durable. If `journal`
  // throws, nothing is sent. Throws Error(unavailable) unless ready. After a
  // successful journal the order is always tracked: it is returned rejected
  // when it could not be sent, and otherwise reflects the state at return;
  // reports arrive later. Provider error codes are negative for local
  // failures (-1003: session ended before sending).
  virtual BrokerOrder submit(const LimitOrder& order, Offset offset,
                             const std::function<void(const BrokerOrder&)>& journal) = 0;
  // Requests cancellation of a working order; the outcome arrives as a report.
  virtual void cancel(const std::string& order_id) = 0;
  virtual BrokerSnapshot snapshot() const = 0;
  virtual void disconnect() = 0;
};
} // namespace asterion
