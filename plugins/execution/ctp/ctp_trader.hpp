#pragma once
#include <asterion/domain/broker_execution.hpp>
#include <filesystem>
#include <map>
#include <memory>
namespace asterion::ctp {
// Credentials stay in memory only while connected, for automatic re-login
// after the SDK reconnects. Nothing here is persisted or logged.
struct TraderConfiguration {
  std::string front, broker, user, password, app_id, auth_code;
};
// CTP 6.7.7 TraderApi execution plugin. It authenticates, logs in, confirms
// the settlement statement (required by CTP before trading each day), then
// synchronizes orders, trades, positions and funds before reporting ready.
// All SDK requests run on one worker thread, never under the state mutex;
// queries are serialized and spaced by the CTP flow limit (one per second).
class Trader final : public BrokerExecutionPort {
public:
  Trader(const std::filesystem::path& library, const std::filesystem::path& flow);
  ~Trader() override;
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override;
  // `known` maps broker keys to caller order IDs from the caller's journal, so
  // orders from earlier sessions stay attributed after a reconnect.
  void connect(TraderConfiguration config, std::map<std::string, std::string> known = {});
  BrokerOrder submit(const LimitOrder& order, Offset offset,
                     const std::function<void(const BrokerOrder&)>& journal) override;
  void cancel(const std::string& order_id) override;
  BrokerSnapshot snapshot() const override;
  // Queries the account's margin and commission rates for each contract with
  // its product code; results arrive in snapshot().costs. Requires ready.
  void query_costs(const std::vector<std::pair<InstrumentId, std::string>>& contracts);
  // Queries the contract's current market and waits for the answer; nothing
  // when the broker has none, the query fails or times out. Requires ready.
  std::optional<BrokerQuote> quote(const InstrumentId& instrument);
  void disconnect() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ctp
