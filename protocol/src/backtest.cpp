#include <asterion/domain/daily_bars.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/backtest.hpp>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>
namespace asterion::protocol {
namespace {
// Index lists: every index names a contract, none twice across all lists.
template <class Lists, class Indexes>
void distinct_contracts(const Lists& lists, std::size_t contracts, Indexes indexes) {
  std::vector<bool> used(contracts);
  for (const auto& list : lists)
    for (const auto index : indexes(list)) {
      if (index >= contracts || used[index])
        throw std::invalid_argument("invalid dominant series contracts");
      used[index] = true;
    }
}
void validate_series(const backtest::v1::BacktestInput& input) {
  const auto& contracts = input.paper().contracts();
  std::vector<bool> used(static_cast<std::size_t>(contracts.size()));
  for (const auto& series : input.series()) {
    if (series.rolls().empty())
      throw std::invalid_argument("invalid dominant series schedule");
    const backtest::v1::DominantRoll* previous = nullptr;
    for (const auto& roll : series.rolls()) {
      (void)parse_trading_date(roll.trading_day());
      if (roll.contract() >= static_cast<unsigned>(contracts.size()) || used[roll.contract()] ||
          !roll.has_factor() || roll.factor().units() <= 0)
        throw std::invalid_argument("invalid dominant series schedule");
      used[roll.contract()] = true;
      const auto& contract = contracts[static_cast<int>(roll.contract())].dataset().contract();
      if (previous) {
        const auto& before = contracts[static_cast<int>(previous->contract())].dataset().contract();
        // One product, later months only, each from a later day.
        if (roll.trading_day() <= previous->trading_day() || contract.venue() != before.venue() ||
            contract.product() != before.product() ||
            contract.delivery_month() <= before.delivery_month() ||
            contract.price_increment().units() != before.price_increment().units() ||
            contract.multiplier().units() != before.multiplier().units())
          throw std::invalid_argument("invalid dominant series schedule");
      }
      previous = &roll;
    }
    if (previous->factor().units() != Decimal::parse("1").raw())
      throw std::invalid_argument("invalid dominant series schedule");
  }
}
} // namespace
backtest::v1::BacktestRequest encode_backtest_request(const Json& input) {
  require_fields(input, {"contracts", "deposit", "risk", "strategies", "series", "holdout_from"});
  if (!input.at("strategies").is_array() || input.at("strategies").empty() ||
      input.at("strategies").size() > 32 || !input.at("holdout_from").is_string())
    throw std::invalid_argument("backtest requires 1 to 32 strategies");
  if (!input.at("series").is_array() || !input.at("contracts").is_array() ||
      input.at("contracts").empty() || input.at("contracts").size() > max_portfolio_contracts)
    throw std::invalid_argument("backtest requires 1 to 20 contracts");
  backtest::v1::BacktestRequest result;
  for (const auto& contract : input.at("contracts")) {
    require_fields(contract, {"data", "cost_schedule"});
    auto* item = result.add_contracts();
    *item->mutable_data() = encode_bar_dataset_request(contract.at("data"));
    *item->mutable_cost_schedule() = encode_cost_schedule(contract.at("cost_schedule"));
  }
  for (const auto& members : input.at("series")) {
    if (!members.is_array() || members.size() < 2)
      throw std::invalid_argument("a dominant series requires at least two month contracts");
    auto* series = result.add_series();
    for (const auto& index : members) {
      if (!index.is_number_unsigned())
        throw std::invalid_argument("invalid dominant series contracts");
      series->add_contracts(index.get<unsigned>());
    }
  }
  distinct_contracts(result.series(), static_cast<std::size_t>(result.contracts_size()),
                     [](const auto& series) { return series.contracts(); });
  result.mutable_deposit()->set_units(Decimal::parse(input.at("deposit").get<std::string>()).raw());
  *result.mutable_risk() = encode_risk(input.at("risk"));
  for (const auto& strategy : input.at("strategies"))
    *result.add_strategies() = encode_strategy(strategy);
  result.set_holdout_from(input.at("holdout_from").get<std::string>());
  return result;
}
std::vector<std::string> backtest_trading_days(const backtest::v1::BacktestInput& input) {
  std::set<std::string> days;
  for (const auto& contract : input.paper().contracts())
    for (const auto& day : contract.dataset().days())
      days.insert(day.trading_day());
  return {days.begin(), days.end()};
}
void set_backtest_holdout(backtest::v1::BacktestInput& input, const std::string& from) {
  if (input.strategies_size() < 2) {
    if (!from.empty())
      throw std::invalid_argument("a holdout belongs to a comparison of several strategies");
    return;
  }
  static_cast<void>(parse_trading_date(from));
  const auto days = backtest_trading_days(input);
  const auto first = std::ranges::lower_bound(days, from);
  if (first == days.end())
    throw std::invalid_argument("the holdout begins after the last trading day");
  input.set_holdout_day(*first);
}
std::size_t backtest_work_units(const backtest::v1::BacktestInput& input) {
  std::size_t bars = 0, development = 0;
  for (const auto& contract : input.paper().contracts())
    for (const auto& bar : contract.dataset().bars()) {
      ++bars;
      development += bar.trading_day() < input.holdout_day();
    }
  return bars + (input.strategies_size() > 1
                     ? static_cast<std::size_t>(input.strategies_size()) * development
                     : 0);
}
Json decode_backtest(const backtest::v1::BacktestInput& input, DatasetView view) {
  validate_message(input);
  if (input.version() != 9 || !input.has_paper() || input.strategies().empty() ||
      input.strategies_size() > 32)
    throw std::invalid_argument("incomplete backtest input");
  if (input.dataset_revision() != dataset_revision(input.paper()))
    throw std::invalid_argument("dataset revision does not match input snapshot");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto paper = decode_input(input.paper(), view);
  Json strategies = Json::array();
  std::set<std::string> distinct;
  for (const auto& strategy : input.strategies()) {
    for (const auto& contract : input.paper().contracts())
      validate_strategy(
          strategy, Decimal::from_raw(contract.dataset().contract().quantity_increment().units()));
    if (!distinct.insert(strategy.SerializeAsString()).second)
      throw std::invalid_argument("compared strategies must differ");
    strategies.push_back(decode_strategy(strategy));
  }
  const auto days = backtest_trading_days(input);
  const auto holdout = std::ranges::find(days, input.holdout_day());
  if (input.strategies_size() == 1
          ? !input.holdout_day().empty()
          : holdout == days.end() ||
                static_cast<std::size_t>(holdout - days.begin()) < backtest_development_days)
    throw std::invalid_argument(
        "comparing strategies requires a holdout that begins on a trading day after at least 20 "
        "others; one strategy takes none");
  validate_series(input);
  Json result{{"version", 9},
              {"dataset_revision", input.dataset_revision()},
              {"paper", std::move(paper)},
              {"strategies", std::move(strategies)},
              {"holdout_day", input.holdout_day()}};
  if (input.series_size()) {
    Json all = Json::array();
    for (const auto& series : input.series()) {
      Json rolls = Json::array();
      for (const auto& roll : series.rolls())
        rolls.push_back({{"trading_day", roll.trading_day()},
                         {"contract", roll.contract()},
                         {"factor", Decimal::from_raw(roll.factor().units()).str()}});
      all.push_back(std::move(rolls));
    }
    result["series"] = std::move(all);
  }
  return result;
}
Json decode_backtest_result(const backtest::v1::BacktestResult& result) {
  validate_message(result);
  if (result.version() != 5 || !result.has_account() || !result.has_max_drawdown() ||
      result.equity().empty() || result.settlements().empty() ||
      static_cast<std::size_t>(result.settlements_size()) > max_dataset_bars)
    throw std::invalid_argument("incomplete backtest result");
  Json curve = Json::array(), settlements = Json::array();
  for (const auto& point : result.equity()) {
    if (!point.has_equity() || point.timestamp_ns() < 0 ||
        (point.event() != backtest::v1::TRADE_MARK &&
         point.event() != backtest::v1::DAILY_SETTLEMENT))
      throw std::invalid_argument("invalid backtest equity event");
    curve.push_back(
        {{"timestamp_ns", std::to_string(point.timestamp_ns())},
         {"equity", Decimal::from_raw(point.equity().units()).str()},
         {"event", point.event() == backtest::v1::TRADE_MARK ? "trade" : "settlement"}});
  }
  for (const auto& day : result.settlements()) {
    if (!day.has_balance() || !day.has_equity() || !day.has_realized() || !day.has_fees() ||
        day.contracts().empty())
      throw std::invalid_argument("incomplete settlement result");
    Json contracts = Json::array();
    for (const auto& c : day.contracts()) {
      if (!c.has_price() || !c.has_position_quantity())
        throw std::invalid_argument("incomplete settlement result");
      InstrumentId{c.venue(), c.symbol()}.validate();
      contracts.push_back(
          {{"venue", c.venue()},
           {"symbol", c.symbol()},
           {"price", Decimal::from_raw(c.price().units()).str()},
           {"position_quantity", Decimal::from_raw(c.position_quantity().units()).str()}});
    }
    settlements.push_back({{"trading_day", day.trading_day()},
                           {"timestamp_ns", std::to_string(day.timestamp_ns())},
                           {"balance", Decimal::from_raw(day.balance().units()).str()},
                           {"equity", Decimal::from_raw(day.equity().units()).str()},
                           {"realized", Decimal::from_raw(day.realized().units()).str()},
                           {"fees", Decimal::from_raw(day.fees().units()).str()},
                           {"contracts", std::move(contracts)}});
  }
  auto account = protocol::decode_snapshot(result.account());
  account["mode"] = "backtest";
  account["persistent"] = false;
  Json candidates = Json::array();
  for (const auto& candidate : result.candidates()) {
    if (!std::isfinite(candidate.total_return()) || !std::isfinite(candidate.max_drawdown()) ||
        candidate.max_drawdown() < 0 ||
        (candidate.has_sharpe() && !std::isfinite(candidate.sharpe())))
      throw std::invalid_argument("invalid backtest comparison evidence");
    candidates.push_back(
        {{"total_return", candidate.total_return()},
         {"max_drawdown", candidate.max_drawdown()},
         {"sharpe", candidate.has_sharpe() ? Json(candidate.sharpe()) : Json(nullptr)}});
  }
  if (candidates.empty() ? result.selected() != 0
                         : candidates.size() < 2 || result.selected() >= candidates.size() ||
                               !result.candidates(static_cast<int>(result.selected())).has_sharpe())
    throw std::invalid_argument("invalid backtest comparison evidence");
  // The selected strategy has the highest ratio and is the first to have it.
  for (int i = 0; i < result.candidates_size(); ++i) {
    const auto best = result.candidates(static_cast<int>(result.selected())).sharpe();
    if (const auto& other = result.candidates(i);
        other.has_sharpe() &&
        (other.sharpe() > best ||
         (other.sharpe() == best && static_cast<unsigned>(i) < result.selected())))
      throw std::invalid_argument("invalid backtest comparison evidence");
  }
  return {{"candidates", std::move(candidates)},
          {"selected", result.selected()},
          {"version", result.version()},
          {"dataset_revision", result.dataset_revision()},
          {"engine_version", result.engine_version()},
          {"account", account},
          {"equity", curve},
          {"settlements", settlements},
          {"max_drawdown", Decimal::from_raw(result.max_drawdown().units()).str()}};
}
} // namespace asterion::protocol
