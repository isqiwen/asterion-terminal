#include "engine.hpp"
#include <asterion/protocol/data.hpp>
#include "strategy.hpp"
#include "performance.hpp"
#include <asterion/domain/daily_bars.hpp>
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
constexpr std::size_t none = static_cast<std::size_t>(-1);
// For each contract, the dominant series it is a month of, or none.
std::vector<std::size_t> series_of(const backtest::v1::BacktestInput& input) {
  std::vector<std::size_t> result(static_cast<std::size_t>(input.paper().contracts_size()), none);
  for (int s = 0; s < input.series_size(); ++s)
    for (const auto& roll : input.series(s).rolls())
      result.at(roll.contract()) = static_cast<std::size_t>(s);
  return result;
}
std::vector<bool> sparse(const std::vector<std::size_t>& series) {
  std::vector<bool> result;
  for (const auto item : series)
    result.push_back(item != none);
  return result;
}
// The bar a series strategy sees: prices scaled to the level of the latest
// month and put back on the price grid. Never used for orders or fills.
MarketBar adjusted(MarketBar bar, Decimal factor, Decimal increment) {
  for (auto* price : {&bar.open, &bar.high, &bar.low, &bar.close})
    *price = protocol::dominant_price(*price, factor, increment);
  return bar;
}
} // namespace
void validate(const backtest::v1::BacktestInput& input) {
  static_cast<void>(protocol::decode_backtest(input, protocol::DatasetView::metadata));
  const auto& p = input.paper();
  const auto series = series_of(input);
  std::vector<int> series_bars(static_cast<std::size_t>(input.series_size()));
  std::size_t warmup = 0;
  for (const auto& strategy : input.strategies())
    warmup = std::max(warmup, protocol::strategy_warmup(strategy));
  for (int index = 0; index < p.contracts_size(); ++index) {
    const auto& item = p.contracts(index);
    const auto& c = item.dataset().contract();
    FuturesContract contract{protocol::instrument(c), c.product(), c.delivery_month()};
    contract.validate();
    (void)protocol::cost_schedule(item.cost_schedule());
    for (const auto& strategy : input.strategies())
      if (!CrossSection::defines(strategy))
        static_cast<void>(make_strategy(strategy, contract.instrument));
    // A series is one strategy over all of its months.
    if (series[static_cast<std::size_t>(index)] != none)
      series_bars[series[static_cast<std::size_t>(index)]] += item.dataset().bars_size();
    else if (static_cast<std::size_t>(item.dataset().bars_size()) < warmup)
      throw std::invalid_argument(
          "backtest requires enough bars for the strategy to take a side on every contract");
  }
  for (const auto bars : series_bars)
    if (static_cast<std::size_t>(bars) < warmup)
      throw std::invalid_argument(
          "backtest requires enough bars for the strategy to take a side on every contract");
  // A rule that ranks takes each ordinary contract and each series as one.
  const auto units = static_cast<std::size_t>(std::ranges::count(series, none)) +
                     static_cast<std::size_t>(input.series_size());
  for (const auto& strategy : input.strategies())
    if (CrossSection::defines(strategy)) {
      static_cast<void>(CrossSection(strategy, units));
      // Only a series has a later month to stand against.
      if (strategy.cross().factor() == protocol::v1::TERM_STRUCTURE &&
          std::ranges::count(series, none))
        throw std::invalid_argument("ranking by the term structure needs dominant series");
    }
  decode_order_limits(protocol::decode_risk(p.risk()));
  if (decimal(p.deposit()) <= Decimal{})
    throw std::invalid_argument("backtest requires positive capital");
  static_cast<void>(replay_schedule(paper_portfolio(p), sparse(series)));
}
namespace {
// One strategy replayed over the input's bars: all of them, or only the
// trading days before `until`. A partial replay ends with that day's
// settlement and carries the equity record alone.
backtest::v1::BacktestResult replay(const backtest::v1::BacktestInput& input,
                                    const protocol::v1::Strategy& definition,
                                    const std::string& until, std::stop_token stop,
                                    const std::function<void()>& step,
                                    const risk_providers::Module* pinned) {
  const auto& p = input.paper();
  auto portfolio = paper_portfolio(p);
  const auto series = series_of(input);
  const auto schedule = replay_schedule(portfolio, sparse(series));
  const auto module = pinned ? *pinned : risk_providers::Module::selected();
  auto risk = module.create(decode_order_limits(protocol::decode_risk(p.risk())));
  risk->start();
  // What a rule takes a side on is a unit: each ordinary contract, and each
  // dominant series as one, read from the back-adjusted bars of whichever
  // month is dominant. `unit` says which one a contract belongs to.
  std::vector<std::size_t> unit(series.size());
  std::vector<Instrument> units;
  for (std::size_t c = 0; c < series.size(); ++c)
    if (series[c] == none) {
      unit[c] = units.size();
      units.push_back(portfolio.contracts[c].terms.instrument);
    }
  for (std::size_t c = 0; c < series.size(); ++c)
    if (series[c] != none)
      unit[c] = units.size() + series[c];
  for (const auto& rolls : input.series())
    units.push_back(portfolio.contracts[rolls.rolls(0).contract()].terms.instrument);
  // One strategy for each unit, or one rule that ranks them all; either way
  // they share the account.
  std::unique_ptr<CrossSection> cross;
  std::vector<std::unique_ptr<Strategy>> strategies;
  // What a ranking by the term structure reads: the carry each trading day
  // of each series begins with, where it has a term point.
  std::vector<std::map<std::string, Decimal>> carry;
  if (CrossSection::defines(definition)) {
    cross = std::make_unique<CrossSection>(definition, units.size());
    if (definition.cross().factor() == protocol::v1::TERM_STRUCTURE) {
      std::vector<const protocol::v1::Contract*> contracts;
      for (const auto& contract : p.contracts())
        contracts.push_back(&contract.dataset().contract());
      for (const auto& rolls : input.series())
        carry.push_back(protocol::term_carries(rolls, contracts));
    }
  } else
    for (auto& instrument : units)
      strategies.push_back(make_strategy(definition, std::move(instrument)));
  PaperExecution execution(decimal(p.deposit()), std::move(portfolio.contracts), risk);
  execution.start();
  for (auto& strategy : strategies)
    strategy->start();
  // The last target asked for each unit.
  std::vector<std::optional<Decimal>> wanted(units.size());
  // The contracts that had a bar at the current timestamp and what each
  // decided on: a ranking made once all of them are in decides them again.
  struct Decided {
    std::size_t contract;
    std::string order;
    Decimal close;
    bool leading;
  };
  std::vector<Decided> together;
  const auto holds = [&](std::size_t contract) {
    const auto& id = execution.contract(contract).terms.instrument.id;
    return std::ranges::any_of(execution.account().positions(),
                               [&](const auto& lot) { return lot.instrument == id; });
  };
  backtest::v1::BacktestResult result;
  result.set_version(5);
  result.set_dataset_revision(input.dataset_revision());
  result.set_engine_version(protocol::backtest_engine_version);
  auto peak = decimal(p.deposit());
  Decimal drawdown;
  const auto add_equity = [&](std::int64_t time, backtest::v1::EquityEvent event,
                              const FuturesAccount& account) {
    const auto equity = account.balance() + account.unrealized();
    peak = std::max(peak, equity);
    drawdown = std::max(drawdown, peak - equity);
    auto* point = result.add_equity();
    point->set_timestamp_ns(time);
    point->set_event(event);
    point->mutable_equity()->set_units(equity.raw());
  };
  std::vector<std::optional<PaperExecution::Target>> pending(series.size());
  const auto total = execution.size();
  for (std::size_t index = 0; index < total; ++index) {
    if (stop.stop_requested())
      throw std::runtime_error("backtest cancelled");
    const auto current = execution.event(index);
    const auto& bar = execution.bar(current);
    const auto& instrument = execution.contract(current.contract).terms.instrument.id;
    // Orders placed after a contract's bar N can only fill on its bar N+1;
    // unfilled remainders expire before its strategy decides again.
    auto& intent = pending[current.contract];
    if (const auto member = series[current.contract]; member != none && intent) {
      const auto& rolls = input.series(static_cast<int>(member));
      if (protocol::dominant_roll(rolls, bar.trading_day).contract() != current.contract)
        intent->quantity = Decimal{};
      // Apply the roll and the no-overlapping-months rule at submission.
      else if (intent->quantity != Decimal{} &&
               std::ranges::any_of(rolls.rolls(), [&](const auto& other) {
                 return other.contract() != current.contract && holds(other.contract());
               }))
        intent.reset();
    }
    execution.advance(intent);
    intent.reset();
    execution.cancel_open_orders(instrument);
    const auto& account = execution.account();
    const auto& event = schedule.event(index);
    const auto order = "strategy." + std::to_string(index);
    const auto decide = [&](Decimal target) {
      intent = PaperExecution::Target{order, target, bar.close};
    };
    const auto member = series[current.contract];
    const auto* roll =
        member == none
            ? nullptr
            : &protocol::dominant_roll(input.series(static_cast<int>(member)), bar.trading_day);
    // The contract its unit trades now: an ordinary one, or the dominant
    // month. A month that is no longer dominant only closes what it holds.
    const bool leading = !roll || roll->contract() == current.contract;
    if (leading) {
      const auto u = unit[current.contract];
      const auto observe = [&](const MarketBar& seen) {
        if (!cross) {
          if (const auto target = strategies[u]->on_bar(seen))
            wanted[u] = target;
        } else if (carry.empty()) {
          cross->on_bar(u, seen.timestamp_ns, seen.close);
        } else if (const auto term = carry[member].find(bar.trading_day);
                   term != carry[member].end()) {
          // A day without a term point is no bar of this unit for the ranking.
          cross->on_bar(u, seen.timestamp_ns, term->second);
        }
      };
      if (roll)
        observe(adjusted(bar, decimal(roll->factor()),
                         execution.contract(current.contract).terms.instrument.price_increment));
      else
        observe(bar);
      if (wanted[u])
        decide(*wanted[u]);
    } else {
      decide(Decimal{});
    }
    if (cross) {
      together.push_back({current.contract, order, bar.close, leading});
      if (index + 1 == total ||
          execution.bar(execution.event(index + 1)).timestamp_ns != bar.timestamp_ns) {
        if (const auto sides = cross->rank()) {
          // A unit holds the lots of the month it trades now whose value at
          // this close is nearest the rule's notional.
          const auto notional = decimal(definition.cross().notional());
          for (const auto& item : together) {
            const auto u = unit[item.contract];
            if (item.leading) {
              const auto& terms = execution.contract(item.contract).terms.instrument;
              const auto lots = quantize(
                  divide(notional, multiply(item.close, terms.multiplier, Rounding::half_up),
                         Rounding::half_up),
                  terms.quantity_increment, Rounding::half_up);
              wanted[u] = (*sides)[u] > 0 ? lots : (*sides)[u] < 0 ? Decimal{} - lots : Decimal{};
            }
          }
          for (const auto& item : together)
            pending[item.contract] = PaperExecution::Target{
                item.order, item.leading ? *wanted[unit[item.contract]] : Decimal{}, item.close};
        }
        together.clear();
      }
    }
    add_equity(bar.timestamp_ns, backtest::v1::TRADE_MARK, account);
    if (event.day_end) {
      const auto& day = schedule.day(event.day);
      execution.cancel_open_orders();
      execution.settle_scheduled(day.prices, index + 1 == total);
      add_equity(bar.timestamp_ns, backtest::v1::DAILY_SETTLEMENT, account);
      auto* settled = result.add_settlements();
      settled->set_trading_day(day.trading_day);
      settled->set_timestamp_ns(bar.timestamp_ns);
      settled->mutable_balance()->set_units(account.balance().raw());
      settled->mutable_equity()->set_units((account.balance() + account.unrealized()).raw());
      settled->mutable_realized()->set_units(account.realized().raw());
      settled->mutable_fees()->set_units(account.fees().raw());
      for (std::size_t c = 0; c < account.contracts().size(); ++c) {
        // A month of a series has no row on the days it does not trade.
        if (!day.prices[c])
          continue;
        const auto& id = account.contracts()[c].instrument.id;
        // Net lots: long positive, short negative.
        Decimal quantity;
        for (const auto& lot : account.positions())
          if (lot.instrument == id)
            quantity = lot.side == Side::buy ? quantity + lot.quantity : quantity - lot.quantity;
        auto* row = settled->add_contracts();
        row->set_venue(id.venue);
        row->set_symbol(id.symbol);
        row->mutable_price()->set_units(day.prices[c]->raw());
        row->mutable_position_quantity()->set_units(quantity.raw());
      }
    }
    step();
    if (event.day_end && !until.empty() &&
        (index + 1 == total || schedule.day(schedule.event(index + 1).day).trading_day >= until))
      return result;
  }
  auto account = execution.snapshot();
  const auto manifest = protocol::decode_input(p, protocol::DatasetView::metadata);
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
    strategy->stop();
  execution.stop();
  return result;
}
} // namespace
backtest::v1::BacktestResult run(const backtest::v1::BacktestInput& input, std::stop_token stop,
                                 const std::function<void(std::size_t, std::size_t)>& progress,
                                 const risk_providers::Module* pinned) {
  validate(input);
  const auto units = protocol::backtest_work_units(input);
  std::size_t completed = 0;
  const auto step = [&] {
    if (progress)
      progress(++completed, units);
  };
  if (input.strategies_size() == 1)
    return replay(input, input.strategies(0), {}, stop, step, pinned);
  // Each strategy is judged by the days before the holdout and nothing else:
  // its replay ends there.
  std::vector<backtest::v1::BacktestCandidate> candidates;
  std::optional<std::size_t> selected;
  for (const auto& strategy : input.strategies()) {
    const auto development = replay(input, strategy, input.holdout_day(), stop, step, pinned);
    std::vector<EquityDay> days;
    for (const auto& day : development.settlements())
      days.push_back(
          {std::chrono::sys_days(parse_trading_date(day.trading_day())), decimal(day.equity())});
    std::vector<Decimal> marks;
    for (const auto& point : development.equity())
      marks.push_back(decimal(point.equity()));
    const auto record = performance(decimal(input.paper().deposit()), days, marks);
    auto& candidate = candidates.emplace_back();
    candidate.set_total_return(record.total_return);
    candidate.set_max_drawdown(record.max_drawdown);
    if (record.sharpe)
      candidate.set_sharpe(*record.sharpe);
    // The highest ratio wins; among equals, the one named first.
    if (record.sharpe && (!selected || *record.sharpe > candidates[*selected].sharpe()))
      selected = candidates.size() - 1;
  }
  if (!selected)
    throw std::invalid_argument(
        "no compared strategy has a development Sharpe ratio: none of them varied before the "
        "holdout");
  auto result =
      replay(input, input.strategies(static_cast<int>(*selected)), {}, stop, step, pinned);
  for (auto& candidate : candidates)
    *result.add_candidates() = std::move(candidate);
  result.set_selected(static_cast<unsigned>(*selected));
  return result;
}
} // namespace asterion::backtest
