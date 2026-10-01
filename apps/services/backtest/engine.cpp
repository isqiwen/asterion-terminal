#include "engine.hpp"
#include <asterion/protocol/data.hpp>
#include "moving_average.hpp"
#include "order_limits.hpp"
#include "risk_module.hpp"
#include "paper_execution.hpp"
#include "portfolio.hpp"
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
  for (const auto& item : p.contracts()) {
    const auto& c = item.dataset().contract();
    FuturesContract contract{protocol::instrument(c), c.product(), c.delivery_month()};
    contract.validate();
    (void)protocol::cost_schedule(item.cost_schedule());
    const MovingAverage strategy(contract.instrument, input.sma().fast(), input.sma().slow(),
                                 decimal(input.sma().quantity()));
    (void)strategy;
    if (item.dataset().bars_size() < static_cast<int>(input.sma().slow()))
      throw std::invalid_argument("backtest requires at least slow bars for every contract");
  }
  decode_order_limits(protocol::decode_risk(p.risk()));
  if (decimal(p.deposit()) <= Decimal{})
    throw std::invalid_argument("backtest requires positive capital");
  static_cast<void>(replay_schedule(paper_portfolio(p)));
}
research::v1::BacktestResult run(const research::v1::BacktestInput& input, std::stop_token stop,
                                 const std::function<void(std::size_t, std::size_t)>& progress,
                                 const risk_providers::Module* pinned) {
  validate(input);
  const auto& p = input.paper();
  auto portfolio = paper_portfolio(p);
  const auto schedule = replay_schedule(portfolio);
  const auto module = pinned ? *pinned : risk_providers::Module::selected();
  auto risk = module.create(decode_order_limits(protocol::decode_risk(p.risk())));
  risk->start();
  // One SMA per contract on that contract's bars; all share the account.
  std::vector<MovingAverage> strategies;
  for (const auto& contract : portfolio.contracts)
    strategies.emplace_back(contract.terms.instrument, input.sma().fast(), input.sma().slow(),
                            decimal(input.sma().quantity()));
  PaperExecution execution(decimal(p.deposit()), std::move(portfolio.contracts), risk);
  execution.start();
  for (auto& strategy : strategies)
    strategy.start();
  research::v1::BacktestResult result;
  result.set_version(5);
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
  const auto total = execution.size();
  for (std::size_t index = 0; index < total; ++index) {
    if (stop.stop_requested())
      throw std::runtime_error("backtest cancelled");
    const auto current = execution.event(index);
    const auto& bar = execution.bar(current);
    const auto& instrument = execution.contract(current.contract).terms.instrument.id;
    // Orders placed after a contract's bar N can only fill on its bar N+1;
    // unfilled remainders expire before its strategy decides again.
    execution.advance();
    execution.cancel_open_orders(instrument);
    const auto& account = execution.account();
    const auto target = strategies[current.contract].on_bar(bar);
    const auto& event = schedule.event(index);
    if (target && !event.day_end)
      execution.reconcile_long_target("sma." + std::to_string(index), instrument, *target,
                                      bar.close);
    add_equity(bar.timestamp_ns, research::v1::TRADE_MARK, account);
    if (event.day_end) {
      const auto& day = schedule.day(event.day);
      execution.cancel_open_orders();
      if (index + 1 == total)
        execution.settle(day.prices);
      else
        execution.settle_day_end(day.prices);
      add_equity(bar.timestamp_ns, research::v1::DAILY_SETTLEMENT, account);
      auto* settled = result.add_settlements();
      settled->set_trading_day(day.trading_day);
      settled->set_timestamp_ns(bar.timestamp_ns);
      settled->mutable_balance()->set_units(account.balance().raw());
      settled->mutable_equity()->set_units((account.balance() + account.unrealized()).raw());
      settled->mutable_realized()->set_units(account.realized().raw());
      settled->mutable_fees()->set_units(account.fees().raw());
      for (std::size_t c = 0; c < account.contracts().size(); ++c) {
        const auto& id = account.contracts()[c].instrument.id;
        Decimal quantity;
        for (const auto& lot : account.positions())
          if (lot.instrument == id)
            quantity = quantity + lot.quantity;
        auto* row = settled->add_contracts();
        row->set_venue(id.venue);
        row->set_symbol(id.symbol);
        row->mutable_price()->set_units(day.prices[c].raw());
        row->mutable_position_quantity()->set_units(quantity.raw());
      }
    }
    if (progress)
      progress(index + 1, total);
  }
  auto account = execution.snapshot();
  const auto manifest = protocol::decode_input(p);
  Json contracts = Json::array();
  for (std::size_t c = 0; c < manifest.at("contracts").size(); ++c) {
    const auto& item = manifest.at("contracts").at(c);
    contracts.push_back({{"contract", item.at("dataset").at("contract")},
                         {"costs", protocol::decode_costs(protocol::encode_costs(
                                       execution.account().contracts()[c].costs))},
                         {"cost_schedule", item.at("cost_schedule")},
                         {"mark", account.at("marks").at(c).at("mark")}});
  }
  account.erase("marks");
  account["contracts"] = std::move(contracts);
  account["risk"] = manifest.at("risk");
  account["storage_state"] = "ready";
  *result.mutable_account() = protocol::encode_snapshot(account);
  result.mutable_max_drawdown()->set_units(drawdown.raw());
  for (auto& strategy : strategies)
    strategy.stop();
  execution.stop();
  return result;
}
Json result_json(const research::v1::BacktestResult& result) {
  return protocol::decode_backtest_result(result);
}

} // namespace asterion::backtest
