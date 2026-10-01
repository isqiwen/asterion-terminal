#pragma once
#include <asterion/domain/execution_port.hpp>
#include <asterion/domain/risk_port.hpp>
#include <optional>
#include <vector>
namespace asterion {
// Fraction of each bar's volume available to all simulated orders together.
inline const Decimal paper_bar_participation = Decimal::parse("0.1");
// One contract of a replayed portfolio: its terms and its ascending bars.
struct ContractBars {
  ContractTerms terms;
  std::vector<MarketBar> bars;
  // Empty only for explicitly fixed-cost in-memory callers. Persisted inputs
  // always carry a validated, nonempty schedule through the protocol.
  std::vector<FuturesCostVersion> cost_schedule;
};
// Historical bar replay of a futures portfolio. The contracts' bars form one
// event stream ordered by bar time (then contract order); each advance
// consumes one bar of one contract. A working order can only fill on a later
// bar of its own contract than the one it was submitted after: a buy fills
// when the bar's low reaches its limit, at min(open, limit); a sell fills
// when the high reaches it, at max(open, limit). Fills share at most
// paper_bar_participation of that bar's volume.
class PaperExecution final : public ExecutionPort {
public:
  struct Event {
    std::size_t contract, bar;
  };
  PaperExecution(Decimal deposit, std::vector<ContractBars> contracts,
                 std::shared_ptr<const RiskPort> risk);
  PluginDescriptor descriptor() const override;
  void start() override { running_ = true; }
  void stop() noexcept override { running_ = false; }
  void submit(LimitOrder order, Offset offset) override;
  void cancel(const std::string& id) override;
  void advance();
  // One price per contract, ordered as account().contracts().
  void settle(const std::vector<Decimal>& prices);
  // Scheduled replay only: settles after the last event of a trading day,
  // before the next day's first event. No outstanding orders; once per day.
  void settle_day_end(const std::vector<Decimal>& prices);
  void cancel_open_orders();
  // Working orders of one contract only.
  void cancel_open_orders(const InstrumentId& instrument);
  // Long/flat target for one contract, routed through normal account checks.
  void reconcile_long_target(const std::string& order_id, const InstrumentId& instrument,
                             Decimal target, Decimal price);
  Json snapshot() const override;
  const FuturesAccount& account() const noexcept { return account_; }
  // Number of consumed events; the next advance consumes event(cursor).
  std::size_t cursor() const noexcept { return cursor_; }
  std::size_t size() const noexcept { return data_->events.size(); }
  const Event& event(std::size_t index) const { return data_->events.at(index); }
  const MarketBar& bar(const Event& event) const {
    return data_->contracts.at(event.contract).bars.at(event.bar);
  }
  const ContractBars& contract(std::size_t index) const { return data_->contracts.at(index); }
  // Incremented by every successful mutation. Each operation has a strong
  // exception guarantee, so an unchanged revision after a failure proves the
  // engine state is untouched.
  std::uint64_t revision() const noexcept { return revision_; }
  // Timestamp of the last consumed event, if any.
  std::optional<std::int64_t> timestamp_ns() const;

private:
  struct Data {
    std::vector<ContractBars> contracts;
    std::vector<Event> events;
  };
  void require_running() const;
  FuturesAccount account_;
  std::shared_ptr<const RiskPort> risk_;
  std::shared_ptr<const Data> data_;
  std::size_t cursor_ = 0;
  std::uint64_t execution_sequence_ = 0;
  std::string last_settled_day_;
  std::uint64_t revision_ = 0;
  bool running_ = false;
};
} // namespace asterion
