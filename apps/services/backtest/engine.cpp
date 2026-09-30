#include "engine.hpp"
#include <asterion/protocol/data.hpp>
#include "moving_average.hpp"
#include "order_limits.hpp"
#include "risk_module.hpp"
#include "paper_execution.hpp"
#include "replay_schedule.hpp"
#include <algorithm>
#include <asterion/domain/futures.hpp>
#include <stdexcept>
namespace asterion::backtest {
namespace {
Decimal decimal(const protocol::v1::Decimal& value) {
  return Decimal::from_raw(value.units());
}
} // namespace
void validate(const research::v1::BacktestInput& input) {
  static_cast<void>(protocol::decode_backtest(input));
  const auto& p = input.paper();
  const auto& c = p.dataset().contract();
  FuturesContract contract{protocol::instrument(c), c.product(), c.delivery_month()};
  contract.validate();
  protocol::futures_costs(p.costs()).validate();
  decode_order_limits(protocol::decode_risk(p.risk()));
  const MovingAverage strategy(contract.instrument, input.sma().fast(), input.sma().slow(),
                               decimal(input.sma().quantity()));
  (void)strategy;
  if (p.dataset().bars_size() < static_cast<int>(input.sma().slow()) ||
      decimal(p.deposit()) <= Decimal{})
    throw std::invalid_argument("backtest requires positive capital and at least slow bars");
  static_cast<void>(PaperReplaySchedule(contract.instrument, protocol::dataset_bars(p.dataset()),
                                        protocol::dataset_days(p.dataset())));
}
research::v1::BacktestResult run(const research::v1::BacktestInput& input, std::stop_token stop,
                                 const std::function<void(std::size_t, std::size_t)>& progress,
                                 const risk_providers::Module* pinned) {
  validate(input);
  const auto& p = input.paper();
  const auto spec = protocol::instrument(p.dataset().contract());
  const auto data = protocol::dataset_bars(p.dataset());
  const PaperReplaySchedule schedule(spec, data, protocol::dataset_days(p.dataset()));
  const auto module = pinned ? *pinned : risk_providers::Module::selected();
  auto risk = module.create(decode_order_limits(protocol::decode_risk(p.risk())));
  risk->start();
  PaperExecution execution(spec, decimal(p.deposit()), protocol::futures_costs(p.costs()), data,
                           risk);
  MovingAverage strategy(spec, input.sma().fast(), input.sma().slow(),
                         decimal(input.sma().quantity()));
  execution.start();
  strategy.start();
  research::v1::BacktestResult result;
  result.set_version(4);
  result.set_dataset_revision(input.dataset_revision());
  result.set_engine_version(protocol::backtest_engine_version);
  auto peak = decimal(p.deposit());
  Decimal drawdown;
  const auto add_equity = [&](std::int64_t time, research::v1::EquityEvent event,
                              const FuturesAccount& account) {
    const auto equity = account.balance() + account.unrealized();
    peak = std::max(peak, equity);
    drawdown = std::max(drawdown, peak - equity);
    auto* point = result.add_equity();
    point->set_timestamp_ns(time);
    point->set_event(event);
    point->mutable_equity()->set_units(equity.raw());
  };
  for (std::size_t index = 0; index < data.size(); ++index) {
    if (stop.stop_requested())
      throw std::runtime_error("backtest cancelled");
    const auto& bar = data[index];
    // Orders placed after bar N can only fill on bar N+1; unfilled
    // remainders expire before the strategy decides again.
    execution.advance();
    execution.cancel_open_orders();
    const auto& account = execution.account();
    const auto target = strategy.on_bar(bar);
    const auto& event = schedule.event(index);
    if (target && !event.day_end)
      execution.reconcile_long_target("sma." + std::to_string(index), *target, bar.close);
    add_equity(bar.timestamp_ns, research::v1::TRADE_MARK, account);
    if (event.day_end) {
      const auto& day = schedule.day(event.day);
      if (index + 1 == data.size())
        execution.settle(day.settlement_price);
      else
        execution.settle_day_end(day.settlement_price);
      add_equity(bar.timestamp_ns, research::v1::DAILY_SETTLEMENT, account);
      auto* settled = result.add_settlements();
      settled->set_trading_day(day.trading_day);
      settled->set_timestamp_ns(bar.timestamp_ns);
      settled->mutable_price()->set_units(day.settlement_price.raw());
      settled->mutable_balance()->set_units(account.balance().raw());
      settled->mutable_equity()->set_units((account.balance() + account.unrealized()).raw());
      settled->mutable_realized()->set_units(account.realized().raw());
      settled->mutable_fees()->set_units(account.fees().raw());
      Decimal quantity;
      for (const auto& lot : account.positions())
        quantity = quantity + lot.quantity;
      settled->mutable_position_quantity()->set_units(quantity.raw());
    }
    if (progress)
      progress(index + 1, data.size());
  }
  auto account = execution.snapshot();
  const auto manifest = protocol::decode_input(p);
  account["contract"] = manifest.at("dataset").at("contract");
  account["costs"] = manifest.at("costs");
  account["risk"] = manifest.at("risk");
  account["storage_state"] = "ready";
  *result.mutable_account() = protocol::encode_snapshot(account);
  result.mutable_max_drawdown()->set_units(drawdown.raw());
  strategy.stop();
  execution.stop();
  return result;
}
Json result_json(const research::v1::BacktestResult& result) {
  return protocol::decode_backtest_result(result);
}

} // namespace asterion::backtest
