#include <asterion/domain/daily_bars.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/backtest.hpp>
#include <charconv>
#include <stdexcept>
namespace asterion::protocol {
namespace {
backtest::v1::SmaStrategy sma(const Json& value) {
  require_fields(value, {"fast", "slow", "quantity"});
  for (auto name : {"fast", "slow"})
    if (!value.at(name).is_number_integer() || value.at(name) < 1 || value.at(name) > 10000)
      throw std::invalid_argument("invalid SMA period");
  backtest::v1::SmaStrategy result;
  result.set_fast(value.at("fast").get<unsigned>());
  result.set_slow(value.at("slow").get<unsigned>());
  result.mutable_quantity()->set_units(
      Decimal::parse(value.at("quantity").get<std::string>()).raw());
  return result;
}
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
  require_fields(input, {"contracts", "deposit", "risk", "sma", "series"});
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
  *result.mutable_sma() = sma(input.at("sma"));
  return result;
}
Json decode_backtest(const backtest::v1::BacktestInput& input, DatasetView view) {
  validate_message(input);
  if (input.version() != 8 || !input.has_paper() || !input.has_sma() || !input.sma().has_quantity())
    throw std::invalid_argument("incomplete backtest input");
  if (input.dataset_revision() != dataset_revision(input.paper()))
    throw std::invalid_argument("dataset revision does not match input snapshot");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto paper = decode_input(input.paper(), view);
  const auto quantity = Decimal::from_raw(input.sma().quantity().units());
  for (const auto& contract : input.paper().contracts())
    if (!input.sma().fast() || input.sma().fast() >= input.sma().slow() ||
        input.sma().slow() > 10000 || quantity <= Decimal{} ||
        !quantity.multiple_of(
            Decimal::from_raw(contract.dataset().contract().quantity_increment().units())))
      throw std::invalid_argument(
          "SMA requires 0 < fast < slow <= 10000 and a positive lot-aligned quantity");
  validate_series(input);
  Json result{{"version", 8},
              {"dataset_revision", input.dataset_revision()},
              {"paper", std::move(paper)},
              {"sma",
               {{"fast", input.sma().fast()},
                {"slow", input.sma().slow()},
                {"quantity", Decimal::from_raw(input.sma().quantity().units()).str()}}}};
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
  return {{"version", result.version()},
          {"dataset_revision", result.dataset_revision()},
          {"engine_version", result.engine_version()},
          {"account", account},
          {"equity", curve},
          {"settlements", settlements},
          {"max_drawdown", Decimal::from_raw(result.max_drawdown().units()).str()}};
}
} // namespace asterion::protocol
