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
  std::vector<const v1::Contract*> contracts;
  for (const auto& contract : input.paper().contracts())
    contracts.push_back(&contract.dataset().contract());
  // No month belongs to two series.
  std::vector<bool> used(contracts.size());
  for (const auto& series : input.series()) {
    validate_dominant_schedule(series, contracts);
    for (const auto& roll : series.rolls()) {
      if (used[roll.contract()])
        throw std::invalid_argument("invalid dominant series schedule");
      used[roll.contract()] = true;
    }
  }
}
} // namespace
backtest::v1::BacktestRequest encode_backtest_request(const Json& input) {
  require_fields(input, {"contracts", "deposit", "risk", "strategies", "series", "holdout_from",
                         "walk_forward"});
  if (!input.at("strategies").is_array() || input.at("strategies").empty() ||
      input.at("strategies").size() > 32 || !input.at("holdout_from").is_string())
    throw std::invalid_argument("backtest requires 1 to 32 strategies");
  if (!input.at("series").is_array() || !input.at("contracts").is_array() ||
      input.at("contracts").empty() || input.at("contracts").size() > max_portfolio_contracts)
    throw std::invalid_argument("backtest requires 1 to 20 contracts");
  backtest::v1::BacktestRequest result;
  for (const auto& contract : input.at("contracts")) {
    require_fields(contract, {"data", "cost_schedule", "slippage_ticks"});
    if (!contract.at("slippage_ticks").is_number_integer() || contract.at("slippage_ticks") < 0 ||
        contract.at("slippage_ticks") > max_slippage_ticks)
      throw std::invalid_argument("slippage is 0 to 100 price increments");
    auto* item = result.add_contracts();
    *item->mutable_data() = encode_bar_dataset_request(contract.at("data"));
    *item->mutable_cost_schedule() = encode_cost_schedule(contract.at("cost_schedule"));
    item->set_slippage_ticks(contract.at("slippage_ticks").get<unsigned>());
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
  if (const auto& rolling = input.at("walk_forward"); !rolling.is_null()) {
    require_fields(rolling, {"training_days", "validation_days"});
    if (!rolling.at("training_days").is_number_unsigned() ||
        !rolling.at("validation_days").is_number_unsigned())
      throw std::invalid_argument("a rolling comparison counts whole trading days");
    result.mutable_walk_forward()->set_training_days(rolling.at("training_days").get<unsigned>());
    result.mutable_walk_forward()->set_validation_days(
        rolling.at("validation_days").get<unsigned>());
  }
  return result;
}
std::vector<std::string> backtest_trading_days(const backtest::v1::BacktestInput& input) {
  std::set<std::string> days;
  for (const auto& contract : input.paper().contracts())
    for (const auto& day : contract.dataset().days())
      days.insert(day.trading_day());
  return {days.begin(), days.end()};
}
void set_backtest_strategies(backtest::v1::BacktestInput& input,
                             const backtest::v1::BacktestRequest& request) {
  *input.mutable_strategies() = request.strategies();
  const auto& from = request.holdout_from();
  if (input.strategies_size() < 2) {
    if (!from.empty() || request.has_walk_forward())
      throw std::invalid_argument("a holdout belongs to a comparison of several strategies");
    return;
  }
  if (request.has_walk_forward()) {
    if (!from.empty())
      throw std::invalid_argument("strategies are compared before a holdout or by rolling");
    *input.mutable_walk_forward() = request.walk_forward();
    return;
  }
  static_cast<void>(parse_trading_date(from));
  const auto days = backtest_trading_days(input);
  const auto first = std::ranges::lower_bound(days, from);
  if (first == days.end())
    throw std::invalid_argument("the holdout begins after the last trading day");
  input.set_holdout_day(*first);
}
std::vector<BacktestRound> backtest_folds(const backtest::v1::BacktestInput& input) {
  const auto days = backtest_trading_days(input).size();
  const std::size_t training = input.walk_forward().training_days();
  const std::size_t validation = input.walk_forward().validation_days();
  std::vector<BacktestRound> result;
  for (auto first = training; first < days; first += validation)
    result.push_back({first, std::min(days, first + validation)});
  return result;
}
std::size_t backtest_work_units(const backtest::v1::BacktestInput& input) {
  std::size_t bars = 0, development = 0;
  for (const auto& contract : input.paper().contracts())
    for (const auto& bar : contract.dataset().bars()) {
      ++bars;
      development += bar.trading_day() < input.holdout_day();
    }
  const auto compared = static_cast<std::size_t>(input.strategies_size());
  if (compared < 2)
    return bars;
  return bars + compared * (input.has_walk_forward() ? bars : development);
}
Json decode_backtest(const backtest::v1::BacktestInput& input, DatasetView view) {
  validate_message(input);
  if (input.version() != 10 || !input.has_paper() || input.strategies().empty() ||
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
  if (input.has_walk_forward()) {
    // Rounds score by a yearly ratio, which fewer than 20 days do not have.
    const auto& rolling = input.walk_forward();
    if (input.strategies_size() == 1 || !input.holdout_day().empty() ||
        rolling.training_days() < backtest_development_days || !rolling.validation_days() ||
        rolling.training_days() >= days.size() || backtest_folds(input).size() > 100)
      throw std::invalid_argument(
          "a rolling comparison of several strategies requires at least 20 training days, "
          "validation days after them and at most 100 rounds, and no holdout");
  } else if (input.strategies_size() == 1
                 ? !input.holdout_day().empty()
                 : holdout == days.end() ||
                       static_cast<std::size_t>(holdout - days.begin()) < backtest_development_days)
    throw std::invalid_argument(
        "comparing strategies requires a holdout that begins on a trading day after at least 20 "
        "others; one strategy takes none");
  validate_series(input);
  Json result{
      {"version", 10},
      {"dataset_revision", input.dataset_revision()},
      {"paper", std::move(paper)},
      {"strategies", std::move(strategies)},
      {"holdout_day", input.holdout_day()},
      {"walk_forward", input.has_walk_forward()
                           ? Json{{"training_days", input.walk_forward().training_days()},
                                  {"validation_days", input.walk_forward().validation_days()}}
                           : Json(nullptr)}};
  if (input.series_size()) {
    Json all = Json::array();
    for (const auto& series : input.series())
      all.push_back(decode_dominant_schedule(series));
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
  // The scores of what was compared, and the highest ratio among them: the
  // first to have it, or none when no score has a ratio.
  const auto scores = [](const auto& compared, Json& shown) {
    std::optional<unsigned> best;
    for (int i = 0; i < compared.size(); ++i) {
      const auto& candidate = compared[i];
      if (!std::isfinite(candidate.total_return()) || !std::isfinite(candidate.max_drawdown()) ||
          candidate.max_drawdown() < 0 ||
          (candidate.has_sharpe() && !std::isfinite(candidate.sharpe())))
        throw std::invalid_argument("invalid backtest comparison evidence");
      shown.push_back(
          {{"total_return", candidate.total_return()},
           {"max_drawdown", candidate.max_drawdown()},
           {"sharpe", candidate.has_sharpe() ? Json(candidate.sharpe()) : Json(nullptr)}});
      if (candidate.has_sharpe() &&
          (!best || candidate.sharpe() > compared[static_cast<int>(*best)].sharpe()))
        best = static_cast<unsigned>(i);
    }
    return best;
  };
  Json candidates = Json::array();
  const auto best = scores(result.candidates(), candidates);
  // The selected strategy is the best of those compared before a holdout.
  if (candidates.empty() ? result.selected() != 0
                         : candidates.size() < 2 || best != result.selected())
    throw std::invalid_argument("invalid backtest comparison evidence");
  Json folds = Json::array();
  for (const auto& fold : result.folds()) {
    static_cast<void>(parse_trading_date(fold.first_day()));
    Json compared = Json::array();
    const auto chosen = scores(fold.candidates(), compared);
    // A round follows its best strategy, or none when none has a ratio.
    if (compared.size() < 2 || !candidates.empty() ||
        chosen != (fold.has_selected() ? std::optional(fold.selected()) : std::nullopt))
      throw std::invalid_argument("invalid backtest comparison evidence");
    folds.push_back({{"first_day", fold.first_day()},
                     {"candidates", std::move(compared)},
                     {"selected", chosen ? Json(*chosen) : Json(nullptr)}});
  }
  return {{"candidates", std::move(candidates)},
          {"selected", result.selected()},
          {"folds", std::move(folds)},
          {"version", result.version()},
          {"dataset_revision", result.dataset_revision()},
          {"engine_version", result.engine_version()},
          {"account", account},
          {"equity", curve},
          {"settlements", settlements},
          {"max_drawdown", Decimal::from_raw(result.max_drawdown().units()).str()}};
}
} // namespace asterion::protocol
