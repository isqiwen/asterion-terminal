#pragma once
#include "ctp_order_identity.hpp"
#include <asterion/domain/broker_execution.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <functional>
namespace asterion::ctp {
// Credentials stay in memory only while connected, for automatic re-login
// after the SDK reconnects. Nothing here is persisted or logged.
struct TraderConfiguration {
  std::string front, broker, user, password, app_id, auth_code;
};
// CTP 6.7.7 TraderApi execution plugin. It authenticates, logs in, confirms
// the settlement statement (required by CTP before trading each day), then
// synchronizes orders, trades, positions and funds before reporting ready.
// SDK creation, initialization, requests and release share one fixed thread;
// no vendor call holds the state mutex. Connection changes are asynchronous;
// queries are serialized and spaced by the CTP flow limit (one per second).
// The host serializes mutations and poll() on its account owner. SDK state
// operations return through one reserved slot; callbacks only copy events.
class Trader final {
public:
  Trader(const std::filesystem::path& library, const std::filesystem::path& flow,
         BrokerSendGate& gate, std::function<void()> events_ready);
  ~Trader();
  void start();
  void stop() noexcept;
  // Durable attribution includes trading day; daily broker key reuse cannot
  // bind a new order to a historical caller order ID.
  void connect(TraderConfiguration config, KnownOrders known = {});
  bool restore_orders(std::string_view day, std::uint64_t generation, const KnownOrders& known);
  std::unique_ptr<PreparedBrokerOrder> prepare(const LimitOrder& order, Offset offset,
                                               std::uint64_t connection_generation,
                                               std::uint64_t exposure_revision,
                                               std::chrono::steady_clock::time_point deadline);
  std::future<BrokerDispatchResult> dispatch(std::unique_ptr<PreparedBrokerOrder> prepared,
                                             BrokerSendPermit permit,
                                             std::uint64_t journal_sequence);
  std::future<BrokerDispatchResult> cancel(const std::string& order_id);
  BrokerSnapshot snapshot() const;
  // Reads the current state in place, without copying orders, trades or
  // positions. The reader runs under the trader's state lock: it returns
  // promptly and never calls the trader.
  void observe(const std::function<void(const BrokerSnapshot&)>& reader) const;
  // Queries the account's margin and commission rates for each contract with
  // its product code; results arrive in snapshot().costs. Requires ready.
  void query_costs(const std::vector<std::pair<InstrumentId, std::string>>& contracts);
  // Returns immediately. Empty result means unavailable, invalidated or expired.
  std::future<std::optional<BrokerQuote>> quote(const InstrumentId& instrument);
  // The account owner applies queued callbacks and advances quote deadlines.
  // events_ready wakes that owner; it must not call poll from a producer thread.
  void poll();
  std::chrono::steady_clock::time_point next_deadline() const;
  void disconnect();
  // Freeze exactly the exposure snapshot checked for a policy change.
  // A late broker report or reconnect rejects before closing the connection.
  void disconnect_checked(std::uint64_t generation, std::uint64_t exposure_revision);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ctp
