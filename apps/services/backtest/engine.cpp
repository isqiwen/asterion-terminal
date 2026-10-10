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
#include <cmath>
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
// What one strategy wants of every unit as bars arrive: one strategy for each
// unit, or one rule that ranks them all.
struct Follower {
  const protocol::v1::Strategy* definition;
  std::unique_ptr<CrossSection> cross;
  std::vector<std::unique_ptr<Strategy>> strategies;
  // The last target asked for each unit.
  std::vector<std::optional<Decimal>> wanted;
};
// One account replayed over the input's bars: all of them, or only the
// trading days before `until`; a partial replay ends with that day's
// settlement and carries the equity record alone. Every one of `definitions`
// reads every bar. `followed` says, for each trading day, whose targets the
// account takes: one of them, or none and it holds nothing.
backtest::v1::BacktestResult replay(const backtest::v1::BacktestInput& input,
                                    const std::vector<const protocol::v1::Strategy*>& definitions,
                                    const std::vector<std::optional<std::size_t>>& followed,
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
  // What a ranking by the term structure reads: the carry each trading day
  // of each series begins with, where it has a term point.
  std::vector<std::map<std::string, Decimal>> carry;
  {
    std::vector<const protocol::v1::Contract*> contracts;
    for (const auto& contract : p.contracts())
      contracts.push_back(&contract.dataset().contract());
    for (const auto& rolls : input.series())
      carry.push_back(protocol::term_carries(rolls, contracts));
  }
  std::vector<Follower> followers;
  for (const auto* definition : definitions) {
    auto& follower = followers.emplace_back();
    follower.definition = definition;
    follower.wanted.resize(units.size());
    if (CrossSection::defines(*definition))
      follower.cross = std::make_unique<CrossSection>(*definition, units.size());
    else
      for (const auto& instrument : units)
        follower.strategies.push_back(make_strategy(*definition, instrument));
  }
  PaperExecution execution(decimal(p.deposit()), std::move(portfolio.contracts), risk);
  execution.start();
  for (auto& follower : followers)
    for (auto& strategy : follower.strategies)
      strategy->start();
  // What a contract decided on at a bar. A ranking made once all bars of a
  // timestamp are in decides those contracts again, and so does a day's end
  // after which the account follows another strategy.
  struct Decided {
    std::size_t contract;
    std::string order;
    Decimal close;
    bool leading;
  };
  std::vector<Decided> together;
  std::vector<std::optional<Decided>> today(series.size());
  // What the account wants of a contract while it follows `whom`: the unit's
  // target where the contract is the one its unit trades now, and nothing of
  // a month that is no longer dominant or while it follows no one.
  const auto target = [&](const std::optional<std::size_t>& whom, std::size_t contract,
                          bool leading) -> std::optional<Decimal> {
    if (!leading || !whom)
      return Decimal{};
    return followers[*whom].wanted[unit[contract]];
  };
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
    const auto member = series[current.contract];
    const auto* roll =
        member == none
            ? nullptr
            : &protocol::dominant_roll(input.series(static_cast<int>(member)), bar.trading_day);
    // The contract its unit trades now: an ordinary one, or the dominant
    // month. A month that is no longer dominant only closes what it holds.
    const bool leading = !roll || roll->contract() == current.contract;
    const auto& whom = followed[event.day];
    if (leading) {
      const auto u = unit[current.contract];
      const auto observe = [&](const MarketBar& seen) {
        for (auto& follower : followers) {
          if (!follower.cross) {
            if (const auto wanted = follower.strategies[u]->on_bar(seen))
              follower.wanted[u] = wanted;
          } else if (follower.definition->cross().factor() != protocol::v1::TERM_STRUCTURE) {
            follower.cross->on_bar(u, seen.timestamp_ns, seen.close);
          } else if (const auto term = carry[member].find(bar.trading_day);
                     term != carry[member].end()) {
            // A day without a term point is no bar of this unit for the ranking.
            follower.cross->on_bar(u, seen.timestamp_ns, seen.close, term->second);
          }
        }
      };
      if (roll)
        observe(adjusted(bar, decimal(roll->factor()),
                         execution.contract(current.contract).terms.instrument.price_increment));
      else
        observe(bar);
    }
    const Decided decided{current.contract, order, bar.close, leading};
    // A strategy that has given no target yet leaves the contract alone.
    const auto issue = [&](const Decided& item, const std::optional<std::size_t>& taken) {
      if (const auto quantity = target(taken, item.contract, item.leading))
        pending[item.contract] = PaperExecution::Target{item.order, *quantity, item.close};
    };
    issue(decided, whom);
    today[current.contract] = decided;
    together.push_back(decided);
    if (index + 1 == total ||
        execution.bar(execution.event(index + 1)).timestamp_ns != bar.timestamp_ns) {
      for (std::size_t k = 0; k < followers.size(); ++k) {
        auto& follower = followers[k];
        if (!follower.cross)
          continue;
        const auto shares = follower.cross->rank();
        if (!shares)
          continue;
        // A unit holds the lots of the month it trades now whose value at
        // this close is nearest its share of the rule's notional.
        const auto notional = decimal(follower.definition->cross().notional());
        for (const auto& item : together) {
          if (!item.leading)
            continue;
          const auto u = unit[item.contract];
          const auto share = (*shares)[u];
          const auto& terms = execution.contract(item.contract).terms.instrument;
          const auto lots = quantize(
              divide(multiply(notional,
                              Decimal::from_raw(std::llround(std::abs(share) * 100000000)),
                              Rounding::half_up),
                     multiply(item.close, terms.multiplier, Rounding::half_up), Rounding::half_up),
              terms.quantity_increment, Rounding::half_up);
          follower.wanted[u] = share > 0 ? lots : share < 0 ? Decimal{} - lots : Decimal{};
        }
        // Every contract of this timestamp decided before the ranking was known.
        if (whom == k)
          for (const auto& item : together)
            issue(item, whom);
      }
      together.clear();
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
    if (event.day_end) {
      // From the next day the account may follow another strategy: what that
      // one wanted after today's last bars is what the next bars fill.
      if (index + 1 < total && followed[event.day + 1] != whom)
        for (const auto& item : today)
          if (item)
            issue(*item, followed[event.day + 1]);
      std::ranges::fill(today, std::nullopt);
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
  for (auto& follower : followers)
    for (auto& strategy : follower.strategies)
      strategy->stop();
  execution.stop();
  return result;
}
} // namespace
namespace {
// What a candidate's days before the holdout, or a round's training days,
// showed of it.
backtest::v1::BacktestCandidate scored(Decimal base, std::span<const EquityDay> days,
                                       std::span<const Decimal> marks) {
  backtest::v1::BacktestCandidate candidate;
  // An account already emptied has no return to speak of.
  if (base <= Decimal{})
    return candidate;
  const auto record = performance(base, days, marks);
  candidate.set_total_return(record.total_return);
  candidate.set_max_drawdown(record.max_drawdown);
  if (record.sharpe)
    candidate.set_sharpe(*record.sharpe);
  return candidate;
}
// The highest ratio wins; among equals, the one named first. None when no
// candidate has one.
template <class Candidates> std::optional<std::size_t> best(const Candidates& candidates) {
  std::optional<std::size_t> selected;
  for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
    if (candidates[i].has_sharpe() &&
        (!selected || candidates[i].sharpe() > candidates[static_cast<int>(*selected)].sharpe()))
      selected = static_cast<std::size_t>(i);
  return selected;
}
// The equity record of one replay, by trading day.
struct Curve {
  std::vector<EquityDay> days;
  // Every equity observation in order, and where each day's end.
  std::vector<Decimal> marks;
  std::vector<std::size_t> day_end;
};
Curve curve(const backtest::v1::BacktestResult& result) {
  Curve value;
  for (const auto& point : result.equity()) {
    value.marks.push_back(decimal(point.equity()));
    if (point.event() == backtest::v1::DAILY_SETTLEMENT)
      value.day_end.push_back(value.marks.size());
  }
  for (const auto& day : result.settlements())
    value.days.push_back(
        {std::chrono::sys_days(parse_trading_date(day.trading_day())), decimal(day.equity())});
  return value;
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
  const auto days = protocol::backtest_trading_days(input).size();
  const auto deposit = decimal(input.paper().deposit());
  // One strategy alone on the account, over all the days or those before `until`.
  const auto alone = [&](const protocol::v1::Strategy& strategy, const std::string& until) {
    return replay(input, {&strategy}, std::vector<std::optional<std::size_t>>(days, 0), until, stop,
                  step, pinned);
  };
  if (input.strategies_size() == 1)
    return alone(input.strategies(0), {});
  if (!input.has_walk_forward()) {
    // Each strategy is judged by the days before the holdout and nothing
    // else: its replay ends there.
    std::vector<backtest::v1::BacktestCandidate> candidates;
    for (const auto& strategy : input.strategies()) {
      const auto development = curve(alone(strategy, input.holdout_day()));
      candidates.push_back(scored(deposit, development.days, development.marks));
    }
    const auto selected = best(candidates);
    if (!selected)
      throw std::invalid_argument(
          "no compared strategy has a development Sharpe ratio: none of them varied before the "
          "holdout");
    auto result = alone(input.strategies(static_cast<int>(*selected)), {});
    for (auto& candidate : candidates)
      *result.add_candidates() = std::move(candidate);
    result.set_selected(static_cast<unsigned>(*selected));
    return result;
  }
  // Rolling: every strategy is first replayed alone over all the days. A round
  // reads of those replays its training days and nothing after them, and the
  // account then follows the round's best strategy over its validation days.
  std::vector<Curve> curves;
  std::vector<const protocol::v1::Strategy*> definitions;
  for (const auto& strategy : input.strategies()) {
    curves.push_back(curve(alone(strategy, {})));
    definitions.push_back(&strategy);
  }
  const auto training = input.walk_forward().training_days();
  std::vector<std::optional<std::size_t>> followed(days);
  std::vector<backtest::v1::BacktestFold> folds;
  for (const auto& range : protocol::backtest_folds(input)) {
    auto& fold = folds.emplace_back();
    fold.set_first_day(
        format_trading_date(std::chrono::year_month_day{curves.front().days[range.first].day}));
    const auto begin = range.first - training;
    for (const auto& record : curves) {
      const auto first_mark = begin ? record.day_end[begin - 1] : 0;
      *fold.add_candidates() =
          scored(begin ? record.days[begin - 1].equity : deposit,
                 std::span(record.days).subspan(begin, training),
                 std::span(record.marks)
                     .subspan(first_mark, record.day_end[range.first - 1] - first_mark));
    }
    // A round in which no strategy has a ratio follows none: the account
    // holds nothing through it.
    if (const auto selected = best(fold.candidates())) {
      fold.set_selected(static_cast<unsigned>(*selected));
      std::fill(followed.begin() + static_cast<std::ptrdiff_t>(range.first),
                followed.begin() + static_cast<std::ptrdiff_t>(range.end), selected);
    }
  }
  auto result = replay(input, definitions, followed, {}, stop, step, pinned);
  for (auto& fold : folds)
    *result.add_folds() = std::move(fold);
  return result;
}
} // namespace asterion::backtest
