#include "engine.hpp"
#include "moving_average.hpp"
#include "order_limits.hpp"
#include "paper_execution.hpp"
#include "replay_schedule.hpp"
#include <algorithm>
#include <asterion/domain/futures.hpp>
#include <asterion/domain/trading_schedule.hpp>
namespace asterion::backtest {
namespace {
Decimal decimal(const protocol::v1::Decimal &value) {
  return Decimal::from_raw(value.units());
}
Instrument instrument(const protocol::v1::Contract &c) {
  return {{c.venue(), c.symbol()},
          AssetClass::futures,
          c.currency(),
          decimal(c.price_increment()),
          decimal(c.quantity_increment()),
          decimal(c.multiplier())};
}
FuturesCosts costs(const protocol::v1::Costs &c) {
  return {decimal(c.margin_per_lot()), decimal(c.open_fee()),
          decimal(c.close_today_fee()), decimal(c.close_yesterday_fee())};
}
std::vector<TradeTick> ticks(const protocol::v1::PaperInput &p) {
  std::vector<TradeTick> result;
  result.reserve(static_cast<std::size_t>(p.ticks_size()));
  for (const auto &t : p.ticks())
    result.push_back({{p.contract().venue(), p.contract().symbol()},
                      t.timestamp_ns(),
                      decimal(t.price()),
                      decimal(t.quantity())});
  return result;
}
std::vector<SettlementDay> calendar(const research::v1::BacktestInput &input) {
  std::vector<SettlementDay> days;
  for (const auto &day : input.days()) {
    std::vector<TradingSession> sessions;
    for (const auto &session : day.sessions())
      sessions.push_back({session.begin_ns(), session.end_ns()});
    days.push_back({TradingDaySchedule(day.trading_day(), std::move(sessions)),
                    decimal(day.settlement_price()), day.schedule_source(),
                    day.settlement_source()});
  }
  return days;
}
} // namespace
void validate(const research::v1::BacktestInput &input) {
  static_cast<void>(protocol::decode_backtest(input));
  const auto &p = input.paper();
  const auto &c = p.contract();
  FuturesContract contract{instrument(c), c.product(), c.delivery_month()};
  contract.validate();
  costs(p.costs()).validate();
  decode_order_limits(protocol::decode_risk(p.risk()));
  const MovingAverage strategy(contract.instrument, input.sma().fast(),
                               input.sma().slow(),
                               decimal(input.sma().quantity()));
  (void)strategy;
  if (p.ticks_size() < static_cast<int>(input.sma().slow()) ||
      p.ticks_size() > 10000 || decimal(p.deposit()) <= Decimal{})
    throw std::invalid_argument(
        "backtest requires positive capital and slow..10000 ticks");
  static_cast<void>(
      PaperReplaySchedule(contract.instrument, ticks(p), calendar(input)));
}
research::v1::BacktestResult
run(const research::v1::BacktestInput &input, std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)> &progress) {
  validate(input);
  const auto &p = input.paper();
  const auto spec = instrument(p.contract());
  const auto data = ticks(p);
  const PaperReplaySchedule schedule(spec, data, calendar(input));
  auto risk = std::make_shared<OrderLimits>(
      decode_order_limits(protocol::decode_risk(p.risk())));
  risk->start();
  PaperExecution execution(spec, decimal(p.deposit()), costs(p.costs()), data,
                           risk);
  MovingAverage strategy(spec, input.sma().fast(), input.sma().slow(),
                         decimal(input.sma().quantity()));
  execution.start();
  strategy.start();
  research::v1::BacktestResult result;
  result.set_version(3);
  result.set_dataset_revision(input.dataset_revision());
  result.set_engine_version("asterion.backtest.sma-long-flat.v3");
  auto peak = decimal(p.deposit());
  Decimal drawdown;
  const auto add_equity = [&](std::int64_t time,
                              research::v1::EquityEvent event,
                              const Json &account) {
    const auto equity = Decimal::parse(account.at("equity").get<std::string>());
    peak = std::max(peak, equity);
    drawdown = std::max(drawdown, peak - equity);
    auto *point = result.add_equity();
    point->set_timestamp_ns(time);
    point->set_event(event);
    point->mutable_equity()->set_units(equity.raw());
  };
  for (std::size_t index = 0; index < data.size(); ++index) {
    if (stop.stop_requested())
      throw std::runtime_error("backtest cancelled");
    // Orders generated after tick N can only fill at N+1 or later. Shared
    // per-tick volume and limit-price matching come from the paper plugin.
    execution.advance();
    auto account = execution.snapshot();
    for (const auto &order : account.at("orders"))
      if (order.at("state") == "accepted" ||
          order.at("state") == "partially_filled")
        execution.cancel(order.at("id").get<std::string>());
    const auto target = strategy.on_tick(data[index]);
    if (target && !schedule.event(index).session_end) {
      execution.reconcile_long_target("sma." + std::to_string(index), *target,
                                      data[index].price);
    }
    add_equity(data[index].timestamp_ns, research::v1::TRADE_MARK, account);
    if (schedule.event(index).day_end) {
      const auto &day = input.days(static_cast<int>(schedule.event(index).day));
      const auto boundary = day.sessions().rbegin()->end_ns();
      if (index + 1 == data.size())
        execution.settle(decimal(day.settlement_price()));
      else
        execution.settle_before_next(boundary, decimal(day.settlement_price()));
      account = execution.snapshot();
      add_equity(boundary, research::v1::DAILY_SETTLEMENT, account);
      auto *settled = result.add_settlements();
      settled->set_trading_day(day.trading_day());
      settled->set_timestamp_ns(boundary);
      *settled->mutable_price() = day.settlement_price();
      const auto amount = [&](const char *name) {
        return Decimal::parse(account.at(name).get<std::string>()).raw();
      };
      settled->mutable_balance()->set_units(amount("balance"));
      settled->mutable_equity()->set_units(amount("equity"));
      settled->mutable_realized()->set_units(amount("realized"));
      settled->mutable_fees()->set_units(amount("fees"));
      Decimal quantity;
      for (const auto &lot : account.at("positions"))
        quantity =
            quantity + Decimal::parse(lot.at("quantity").get<std::string>());
      settled->mutable_position_quantity()->set_units(quantity.raw());
    }
    if (progress)
      progress(index + 1, data.size());
  }
  auto account = execution.snapshot();
  const auto manifest = protocol::decode_input(p);
  account["contract"] = manifest.at("contract");
  account["costs"] = manifest.at("costs");
  account["risk"] = manifest.at("risk");
  account["storage_state"] = "ready";
  *result.mutable_account() = protocol::encode_snapshot(account);
  result.mutable_max_drawdown()->set_units(drawdown.raw());
  strategy.stop();
  execution.stop();
  return result;
}
Json result_json(const research::v1::BacktestResult &result) {
  return protocol::decode_backtest_result(result);
}

} // namespace asterion::backtest
