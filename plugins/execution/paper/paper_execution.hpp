#pragma once
#include <asterion/domain/execution_port.hpp>
#include <asterion/domain/risk_port.hpp>
#include <map>
#include <optional>
namespace asterion {
class PaperExecution final : public ExecutionPort {
public:
  PaperExecution(Instrument instrument, Decimal deposit, FuturesCosts costs,
                 std::vector<TradeTick> ticks, std::shared_ptr<const RiskPort> risk);
  PluginDescriptor descriptor() const override;
  void start() override { running_ = true; }
  void stop() noexcept override { running_ = false; }
  void submit(LimitOrder order, Offset offset) override;
  void cancel(const std::string& id) override;
  void advance();
  void settle(Decimal price);
  // Scheduled replay only: caller supplies calendar boundary and settlement
  // price. No outstanding orders; boundary lies after the last consumed tick
  // and at or before the next tick. Repeated boundaries are rejected.
  void settle_before_next(std::int64_t boundary_ns, Decimal price);
  void cancel_open_orders();
  // Single-contract long/flat target, routed through normal account checks.
  void reconcile_long_target(const std::string& order_id, Decimal target, Decimal price);
  Json snapshot() const override;
  const FuturesAccount& account() const noexcept { return account_; }
  // Number of consumed replay events; the next advance consumes ticks[cursor].
  std::size_t cursor() const noexcept { return cursor_; }
  std::size_t size() const noexcept { return ticks_->size(); }
  // Incremented by every successful mutation. Each operation has a strong
  // exception guarantee, so an unchanged revision after a failure proves the
  // engine state is untouched.
  std::uint64_t revision() const noexcept { return revision_; }
  // Timestamp of the last consumed event, if any.
  std::optional<std::int64_t> timestamp_ns() const;

private:
  void require_running() const;
  FuturesAccount account_;
  std::shared_ptr<const RiskPort> risk_;
  std::shared_ptr<const std::vector<TradeTick>> ticks_;
  std::size_t cursor_ = 0;
  std::uint64_t execution_sequence_ = 0;
  std::int64_t last_settlement_boundary_ = -1;
  std::uint64_t revision_ = 0;
  // Estimated quantity still ahead of each resting order at its limit price.
  std::map<std::string, Decimal> queue_;
  bool running_ = false;
};
} // namespace asterion
