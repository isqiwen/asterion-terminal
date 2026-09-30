#pragma once
#include <asterion/domain/execution_port.hpp>
#include <asterion/domain/risk_port.hpp>
#include <optional>
namespace asterion {
// Fraction of each bar's volume available to all simulated orders together.
inline const Decimal paper_bar_participation = Decimal::parse("0.1");
inline constexpr std::size_t paper_max_bars = 20000;
// Historical bar replay. A working order can only fill on a later bar than the
// one it was submitted after: a buy fills when the bar's low reaches its limit,
// at min(open, limit); a sell fills when the high reaches it, at max(open,
// limit). Fills share at most paper_bar_participation of the bar's volume.
class PaperExecution final : public ExecutionPort {
public:
  PaperExecution(Instrument instrument, Decimal deposit, FuturesCosts costs,
                 std::vector<MarketBar> bars, std::shared_ptr<const RiskPort> risk);
  PluginDescriptor descriptor() const override;
  void start() override { running_ = true; }
  void stop() noexcept override { running_ = false; }
  void submit(LimitOrder order, Offset offset) override;
  void cancel(const std::string& id) override;
  void advance();
  void settle(Decimal price);
  // Scheduled replay only: settles after the last bar of a trading day, before
  // the next day's first bar. No outstanding orders; one settlement per day.
  void settle_day_end(Decimal price);
  void cancel_open_orders();
  // Single-contract long/flat target, routed through normal account checks.
  void reconcile_long_target(const std::string& order_id, Decimal target, Decimal price);
  Json snapshot() const override;
  const FuturesAccount& account() const noexcept { return account_; }
  // Number of consumed bars; the next advance consumes bars[cursor].
  std::size_t cursor() const noexcept { return cursor_; }
  std::size_t size() const noexcept { return bars_->size(); }
  const MarketBar& bar(std::size_t index) const { return bars_->at(index); }
  // Incremented by every successful mutation. Each operation has a strong
  // exception guarantee, so an unchanged revision after a failure proves the
  // engine state is untouched.
  std::uint64_t revision() const noexcept { return revision_; }
  // Timestamp of the last consumed bar, if any.
  std::optional<std::int64_t> timestamp_ns() const;

private:
  void require_running() const;
  FuturesAccount account_;
  std::shared_ptr<const RiskPort> risk_;
  std::shared_ptr<const std::vector<MarketBar>> bars_;
  std::size_t cursor_ = 0;
  std::uint64_t execution_sequence_ = 0;
  std::string last_settled_day_;
  std::uint64_t revision_ = 0;
  bool running_ = false;
};
} // namespace asterion
