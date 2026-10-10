#include <asterion/foundation/decimal.hpp>
#include <asterion/domain/futures.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/trading.hpp>
#include <google/protobuf/unknown_field_set.h>
#include <charconv>
#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
namespace asterion::protocol {
namespace {
void set(v1::Decimal* target, const Json& value) {
  target->set_units(Decimal::parse(value.get<std::string>()).raw());
}
std::string get(const v1::Decimal& value) {
  return Decimal::from_raw(value.units()).str();
}
v1::Side side(const std::string& value) {
  if (value == "buy")
    return v1::BUY;
  if (value == "sell")
    return v1::SELL;
  throw std::invalid_argument("invalid side");
}
std::string side(v1::Side value) {
  if (value == v1::BUY)
    return "buy";
  if (value == v1::SELL)
    return "sell";
  throw std::invalid_argument("invalid side");
}
} // namespace
PositionSides position_sides(v1::PositionSides value) {
  switch (value) {
  case v1::LONG_AND_SHORT:
    return PositionSides::both;
  case v1::LONG_ONLY:
    return PositionSides::long_only;
  case v1::SHORT_ONLY:
    return PositionSides::short_only;
  default:
    throw std::invalid_argument("position sides must be both, long or short");
  }
}
v1::PositionSides encode_position_sides(const Json& value) {
  if (!value.is_string())
    throw std::invalid_argument("position sides must be both, long or short");
  switch (parse_position_sides(value.get<std::string>())) {
  case PositionSides::long_only:
    return v1::LONG_ONLY;
  case PositionSides::short_only:
    return v1::SHORT_ONLY;
  case PositionSides::both:
    break;
  }
  return v1::LONG_AND_SHORT;
}
v1::Strategy encode_strategy(const Json& value) {
  if (!value.is_object() || !value.contains("rule") || !value.at("rule").is_object() ||
      !value.at("rule").contains("kind") || !value.at("rule").at("kind").is_string())
    throw std::invalid_argument("invalid strategy");
  const auto& rule = value.at("rule");
  const auto kind = rule.at("kind").get<std::string>();
  // A rule over several contracts carries its own size and takes no quantity.
  const bool ranks = kind == "cross_momentum" || kind == "cross_term_structure";
  if (ranks)
    require_fields(value, {"sides", "rule"});
  else
    require_fields(value, {"quantity", "sides", "rule"});
  const auto amount = [](const Json& text) {
    if (!text.is_string())
      throw std::invalid_argument("invalid strategy");
    return Decimal::parse(text.get<std::string>()).raw();
  };
  v1::Strategy result;
  if (!ranks)
    result.mutable_quantity()->set_units(amount(value.at("quantity")));
  result.set_sides(encode_position_sides(value.at("sides")));
  const auto window = [&](const char* name) {
    if (!rule.at(name).is_number_integer() || rule.at(name) < 0 || rule.at(name) > 10000)
      throw std::invalid_argument("strategy windows are whole bars up to 10000");
    return rule.at(name).get<unsigned>();
  };
  if (kind == "moving_average") {
    require_fields(rule, {"kind", "fast", "slow"});
    result.mutable_moving_average()->set_fast(window("fast"));
    result.mutable_moving_average()->set_slow(window("slow"));
  } else if (kind == "breakout") {
    require_fields(rule, {"kind", "entry", "exit"});
    result.mutable_breakout()->set_entry(window("entry"));
    result.mutable_breakout()->set_exit(window("exit"));
  } else if (kind == "momentum") {
    require_fields(rule, {"kind", "lookback"});
    result.mutable_momentum()->set_lookback(window("lookback"));
  } else if (kind == "reversion") {
    require_fields(rule, {"kind", "window", "width"});
    result.mutable_reversion()->set_window(window("window"));
    result.mutable_reversion()->mutable_width()->set_units(amount(rule.at("width")));
  } else if (ranks) {
    require_fields(rule, {"kind", "reverse", "lookback", "rebalance", "count", "notional"});
    if (!rule.at("reverse").is_boolean())
      throw std::invalid_argument("invalid strategy");
    auto* cross = result.mutable_cross();
    cross->set_factor(kind == "cross_momentum" ? v1::PRICE_MOMENTUM : v1::TERM_STRUCTURE);
    cross->set_reversed(rule.at("reverse").get<bool>());
    cross->set_lookback(window("lookback"));
    cross->set_rebalance(window("rebalance"));
    cross->set_count(window("count"));
    cross->mutable_notional()->set_units(amount(rule.at("notional")));
  } else
    throw std::invalid_argument("unknown strategy rule");
  return result;
}
Json decode_strategy(const v1::Strategy& strategy) {
  Json rule;
  switch (strategy.rule_case()) {
  case v1::Strategy::kMovingAverage:
    rule = {{"kind", "moving_average"},
            {"fast", strategy.moving_average().fast()},
            {"slow", strategy.moving_average().slow()}};
    break;
  case v1::Strategy::kBreakout:
    rule = {{"kind", "breakout"},
            {"entry", strategy.breakout().entry()},
            {"exit", strategy.breakout().exit()}};
    break;
  case v1::Strategy::kMomentum:
    rule = {{"kind", "momentum"}, {"lookback", strategy.momentum().lookback()}};
    break;
  case v1::Strategy::kReversion:
    rule = {{"kind", "reversion"},
            {"window", strategy.reversion().window()},
            {"width", Decimal::from_raw(strategy.reversion().width().units()).str()}};
    break;
  case v1::Strategy::kCross:
    rule = {{"kind", strategy.cross().factor() == v1::TERM_STRUCTURE ? "cross_term_structure"
                                                                     : "cross_momentum"},
            {"reverse", strategy.cross().reversed()},
            {"lookback", strategy.cross().lookback()},
            {"rebalance", strategy.cross().rebalance()},
            {"count", strategy.cross().count()},
            {"notional", Decimal::from_raw(strategy.cross().notional().units()).str()}};
    break;
  case v1::Strategy::RULE_NOT_SET:
    throw std::invalid_argument("unknown strategy rule");
  }
  Json result{{"sides", position_sides_name(position_sides(strategy.sides()))},
              {"rule", std::move(rule)}};
  if (!strategy.has_cross())
    result["quantity"] = Decimal::from_raw(strategy.quantity().units()).str();
  return result;
}
void validate_strategy(const v1::Strategy& strategy, Decimal quantity_increment) {
  static_cast<void>(position_sides(strategy.sides()));
  if (strategy.has_cross()) {
    const auto& rule = strategy.cross();
    if (strategy.has_quantity() ||
        (rule.factor() != v1::PRICE_MOMENTUM && rule.factor() != v1::TERM_STRUCTURE) ||
        !rule.lookback() || rule.lookback() > 10000 || !rule.rebalance() ||
        rule.rebalance() > 10000 || !rule.count() || rule.count() > 10 ||
        rule.notional().units() <= 0)
      throw std::invalid_argument(
          "a ranking rule requires a factor, a lookback and a rebalance of 1..10000 bars, 1..10 "
          "contracts a side and a positive notional in place of a quantity");
    return;
  }
  const auto quantity = Decimal::from_raw(strategy.quantity().units());
  if (!strategy.has_quantity() || quantity <= Decimal{} ||
      !quantity.multiple_of(quantity_increment))
    throw std::invalid_argument("strategy requires a positive lot-aligned quantity");
  switch (strategy.rule_case()) {
  case v1::Strategy::kMovingAverage:
    if (const auto& rule = strategy.moving_average();
        !rule.fast() || rule.fast() >= rule.slow() || rule.slow() > 10000)
      throw std::invalid_argument("moving average requires 0 < fast < slow <= 10000");
    return;
  case v1::Strategy::kBreakout:
    if (const auto& rule = strategy.breakout();
        !rule.exit() || rule.exit() > rule.entry() || rule.entry() > 10000)
      throw std::invalid_argument("breakout requires 1 <= exit <= entry <= 10000");
    return;
  case v1::Strategy::kMomentum:
    if (!strategy.momentum().lookback() || strategy.momentum().lookback() > 10000)
      throw std::invalid_argument("momentum requires a lookback of 1..10000 bars");
    return;
  case v1::Strategy::kReversion: {
    const auto& rule = strategy.reversion();
    const auto width = Decimal::from_raw(rule.width().units());
    if (rule.window() < 2 || rule.window() > 10000 || !rule.has_width() || width <= Decimal{} ||
        width > Decimal::parse("10"))
      throw std::invalid_argument(
          "reversion requires a window of 2..10000 bars and a width above 0 up to 10");
    return;
  }
  case v1::Strategy::kCross:
  case v1::Strategy::RULE_NOT_SET:
    break;
  }
  throw std::invalid_argument("unknown strategy rule");
}
std::size_t strategy_warmup(const v1::Strategy& strategy) {
  switch (strategy.rule_case()) {
  case v1::Strategy::kMovingAverage:
    return strategy.moving_average().slow();
  case v1::Strategy::kBreakout:
    return strategy.breakout().entry() + 1;
  case v1::Strategy::kMomentum:
    return strategy.momentum().lookback() + 1;
  case v1::Strategy::kReversion:
    return strategy.reversion().window();
  case v1::Strategy::kCross:
    // Momentum compares with the bar a lookback earlier; the term structure
    // averages over the lookback itself.
    return strategy.cross().lookback() + (strategy.cross().factor() == v1::PRICE_MOMENTUM ? 1 : 0);
  case v1::Strategy::RULE_NOT_SET:
    break;
  }
  throw std::invalid_argument("unknown strategy rule");
}
namespace {
v1::Offset offset(const std::string& value) {
  if (value == "open")
    return v1::OPEN;
  if (value == "close_today")
    return v1::CLOSE_TODAY;
  if (value == "close_yesterday")
    return v1::CLOSE_YESTERDAY;
  if (value == "close")
    return v1::CLOSE;
  throw std::invalid_argument("invalid offset");
}
std::string offset(v1::Offset value) {
  if (value == v1::OPEN)
    return "open";
  if (value == v1::CLOSE_TODAY)
    return "close_today";
  if (value == v1::CLOSE_YESTERDAY)
    return "close_yesterday";
  if (value == v1::CLOSE)
    return "close";
  throw std::invalid_argument("invalid offset");
}
v1::OrderState state(const std::string& value) {
  if (value == "accepted")
    return v1::ACCEPTED;
  if (value == "partially_filled")
    return v1::PARTIALLY_FILLED;
  if (value == "filled")
    return v1::FILLED;
  if (value == "cancelled")
    return v1::CANCELLED;
  if (value == "rejected")
    return v1::REJECTED;
  throw std::invalid_argument("invalid order state");
}
std::string state(v1::OrderState value) {
  switch (value) {
  case v1::ACCEPTED:
    return "accepted";
  case v1::PARTIALLY_FILLED:
    return "partially_filled";
  case v1::FILLED:
    return "filled";
  case v1::CANCELLED:
    return "cancelled";
  case v1::REJECTED:
    return "rejected";
  default:
    throw std::invalid_argument("invalid order state");
  }
}
v1::Contract contract(const Json& c) {
  require_fields(c, {"venue", "symbol", "currency", "price_increment", "quantity_increment",
                     "multiplier", "product", "delivery_month"});
  v1::Contract result;
  result.set_venue(c.at("venue").get<std::string>());
  result.set_symbol(c.at("symbol").get<std::string>());
  result.set_currency(c.at("currency").get<std::string>());
  result.set_product(c.at("product").get<std::string>());
  result.set_delivery_month(c.at("delivery_month").get<std::string>());
  set(result.mutable_price_increment(), c.at("price_increment"));
  set(result.mutable_quantity_increment(), c.at("quantity_increment"));
  set(result.mutable_multiplier(), c.at("multiplier"));
  return result;
}
Json contract(const v1::Contract& c) {
  return {{"venue", c.venue()},
          {"symbol", c.symbol()},
          {"currency", c.currency()},
          {"price_increment", get(c.price_increment())},
          {"quantity_increment", get(c.quantity_increment())},
          {"multiplier", get(c.multiplier())},
          {"product", c.product()},
          {"delivery_month", c.delivery_month()}};
}
} // namespace
v1::Costs encode_costs(const Json& c) {
  require_fields(c, {"margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee",
                     "margin_rate", "open_fee_rate", "close_today_fee_rate",
                     "close_yesterday_fee_rate"});
  v1::Costs result;
  set(result.mutable_margin_per_lot(), c.at("margin_per_lot"));
  set(result.mutable_open_fee(), c.at("open_fee"));
  set(result.mutable_close_today_fee(), c.at("close_today_fee"));
  set(result.mutable_close_yesterday_fee(), c.at("close_yesterday_fee"));
  set(result.mutable_margin_rate(), c.at("margin_rate"));
  set(result.mutable_open_fee_rate(), c.at("open_fee_rate"));
  set(result.mutable_close_today_fee_rate(), c.at("close_today_fee_rate"));
  set(result.mutable_close_yesterday_fee_rate(), c.at("close_yesterday_fee_rate"));
  futures_costs(result).validate();
  return result;
}
Json decode_costs(const v1::Costs& c) {
  if (!c.has_margin_per_lot() || !c.has_open_fee() || !c.has_close_today_fee() ||
      !c.has_close_yesterday_fee() || !c.has_margin_rate() || !c.has_open_fee_rate() ||
      !c.has_close_today_fee_rate() || !c.has_close_yesterday_fee_rate())
    throw std::invalid_argument("missing explicit paper costs");
  return {{"margin_per_lot", get(c.margin_per_lot())},
          {"open_fee", get(c.open_fee())},
          {"close_today_fee", get(c.close_today_fee())},
          {"close_yesterday_fee", get(c.close_yesterday_fee())},
          {"margin_rate", get(c.margin_rate())},
          {"open_fee_rate", get(c.open_fee_rate())},
          {"close_today_fee_rate", get(c.close_today_fee_rate())},
          {"close_yesterday_fee_rate", get(c.close_yesterday_fee_rate())}};
}
FuturesCosts futures_costs(const v1::Costs& c) {
  const auto value = [](const v1::Decimal& d) { return Decimal::from_raw(d.units()); };
  return {value(c.margin_per_lot()),       value(c.open_fee()),
          value(c.close_today_fee()),      value(c.close_yesterday_fee()),
          value(c.margin_rate()),          value(c.open_fee_rate()),
          value(c.close_today_fee_rate()), value(c.close_yesterday_fee_rate())};
}
v1::Costs encode_costs(const FuturesCosts& costs) {
  costs.validate();
  v1::Costs result;
#define COST(name) result.mutable_##name()->set_units(costs.name.raw())
  COST(margin_per_lot);
  COST(open_fee);
  COST(close_today_fee);
  COST(close_yesterday_fee);
  COST(margin_rate);
  COST(open_fee_rate);
  COST(close_today_fee_rate);
  COST(close_yesterday_fee_rate);
#undef COST
  return result;
}
std::vector<FuturesCostVersion> cost_schedule(const v1::CostSchedule& input) {
  std::vector<FuturesCostVersion> result;
  for (const auto& version : input.versions()) {
    (void)decode_costs(version.values());
    result.push_back({version.effective_from(), version.source(), futures_costs(version.values())});
  }
  validate_cost_schedule(result);
  return result;
}
v1::CostSchedule encode_cost_schedule(const Json& input) {
  if (!input.is_array() || input.empty() || input.size() > 512)
    throw std::invalid_argument("cost schedule requires 1 to 512 versions");
  v1::CostSchedule result;
  for (const auto& version : input) {
    require_fields(version, {"effective_from", "source", "values"});
    auto* row = result.add_versions();
    row->set_effective_from(version.at("effective_from").get<std::string>());
    row->set_source(version.at("source").get<std::string>());
    *row->mutable_values() = encode_costs(version.at("values"));
  }
  (void)cost_schedule(result);
  return result;
}
Json decode_cost_schedule(const v1::CostSchedule& input) {
  (void)cost_schedule(input);
  Json result = Json::array();
  for (const auto& version : input.versions())
    result.push_back({{"effective_from", version.effective_from()},
                      {"source", version.source()},
                      {"values", decode_costs(version.values())}});
  return result;
}
void validate_message(const google::protobuf::Message& message) {
  const auto* reflection = message.GetReflection();
  if (reflection->GetUnknownFields(message).field_count())
    throw std::invalid_argument("unknown Protobuf field");
  // Our proto3 contracts have no extensions. Walk message fields directly;
  // enumerating every populated scalar allocates a vector for each Decimal
  // and bar in large immutable datasets, without contributing validation.
  const auto* descriptor = message.GetDescriptor();
  for (int index = 0; index < descriptor->field_count(); ++index) {
    const auto* field = descriptor->field(index);
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (field->is_repeated()) {
        for (int i = 0; i < reflection->FieldSize(message, field); ++i)
          validate_message(reflection->GetRepeatedMessage(message, field, i));
      } else if (reflection->HasField(message, field))
        validate_message(reflection->GetMessage(message, field));
    }
  }
}
v1::Contract encode_contract(const Json& value) {
  return contract(value);
}
Json decode_contract(const v1::Contract& value) {
  return contract(value);
}
v1::RiskLimits encode_risk(const Json& value) {
  require_fields(value, {"max_order_quantity", "max_gross_quantity", "max_working_orders"});
  if (!value.at("max_working_orders").is_number_unsigned() || value.at("max_working_orders") == 0)
    throw std::invalid_argument("invalid risk working order limit");
  v1::RiskLimits result;
  set(result.mutable_max_order_quantity(), value.at("max_order_quantity"));
  set(result.mutable_max_gross_quantity(), value.at("max_gross_quantity"));
  if (get(result.max_order_quantity()) != value.at("max_order_quantity").get<std::string>() ||
      get(result.max_gross_quantity()) != value.at("max_gross_quantity").get<std::string>())
    throw std::invalid_argument("risk quantities require canonical decimal text");
  if (result.max_order_quantity().units() <= 0 || result.max_gross_quantity().units() <= 0)
    throw std::invalid_argument("risk quantities must be positive");
  result.set_max_working_orders(value.at("max_working_orders").get<std::uint64_t>());
  return result;
}
Json decode_risk(const v1::RiskLimits& value) {
  if (!value.has_max_order_quantity() || !value.has_max_gross_quantity() ||
      value.max_order_quantity().units() <= 0 || value.max_gross_quantity().units() <= 0 ||
      !value.max_working_orders())
    throw std::invalid_argument("missing explicit risk limits");
  return {{"max_order_quantity", get(value.max_order_quantity())},
          {"max_gross_quantity", get(value.max_gross_quantity())},
          {"max_working_orders", value.max_working_orders()}};
}
namespace {
Json instrument_fields(const std::string& venue, const std::string& symbol) {
  InstrumentId{venue, symbol}.validate();
  return {{"venue", venue}, {"symbol", symbol}};
}
} // namespace
// Identity of a portfolio's data: its datasets' revisions in contract order.
std::string dataset_revision(const v1::PaperInput& input) {
  if (input.contracts().empty())
    throw std::invalid_argument("paper input has no contracts");
  Json revisions = Json::array();
  for (const auto& contract : input.contracts()) {
    validate_bar_dataset(contract.dataset());
    revisions.push_back(contract.dataset().revision());
  }
  return input.contracts_size() == 1 ? input.contracts(0).dataset().revision()
                                     : sha256_bytes(revisions.dump());
}
ContractTerms contract_terms(const v1::PaperContract& contract) {
  validate_bar_dataset(contract.dataset());
  return {
      instrument(contract.dataset().contract()),
      costs_on(cost_schedule(contract.cost_schedule()), contract.dataset().bars(0).trading_day())
          .values};
}
// Manifest version 4: each contract pins a complete dated cost schedule.
Json decode_input(const v1::PaperInput& input, DatasetView view) {
  if (!input.has_deposit() || !input.has_risk() || input.contracts().empty() ||
      static_cast<std::size_t>(input.contracts_size()) > max_portfolio_contracts)
    throw std::invalid_argument("paper input requires deposit, risk and 1 to 20 contracts");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto risk = decode_risk(input.risk());
  Json contracts = Json::array();
  std::vector<ContractTerms> terms;
  for (const auto& c : input.contracts()) {
    if (!c.has_dataset() || !c.has_cost_schedule())
      throw std::invalid_argument("missing explicit paper costs or dataset");
    contracts.push_back({{"dataset", decode_bar_dataset(c.dataset(), view)},
                         {"cost_schedule", decode_cost_schedule(c.cost_schedule())}});
    terms.push_back(contract_terms(c));
  }
  // The account validates currency, uniqueness and each contract's terms.
  static_cast<void>(FuturesAccount(Decimal::from_raw(input.deposit().units()), terms));
  return {{"version", 4},
          {"type", "historical_paper"},
          {"deposit", get(input.deposit())},
          {"risk", std::move(risk)},
          {"contracts", std::move(contracts)}};
}
v1::Command encode_command(const Json& c) {
  v1::Command result;
  result.set_request_id(c.at("request_id").get<std::string>());
  const auto action = c.at("action").get<std::string>();
  if (action == "cancel") {
    require_fields(c, {"request_id", "action", "order_id"});
    result.mutable_cancel()->set_order_id(c.at("order_id").get<std::string>());
  } else if (action == "submit") {
    require_fields(c, {"request_id", "action", "order_id", "venue", "symbol", "side", "offset",
                       "quantity", "price"});
    auto* order = result.mutable_submit();
    order->set_order_id(c.at("order_id").get<std::string>());
    order->set_venue(c.at("venue").get<std::string>());
    order->set_symbol(c.at("symbol").get<std::string>());
    InstrumentId{order->venue(), order->symbol()}.validate();
    order->set_side(side(c.at("side").get<std::string>()));
    order->set_offset(offset(c.at("offset").get<std::string>()));
    set(order->mutable_quantity(), c.at("quantity"));
    set(order->mutable_price(), c.at("price"));
  } else if (action == "live_authorize") {
    require_fields(c, {"request_id", "action", "user_id"});
    result.mutable_live_authorize()->set_user_id(c.at("user_id").get<std::string>());
  } else if (action == "live_revoke") {
    require_fields(c, {"request_id", "action"});
    result.mutable_live_revoke();
  } else if (action == "live_policy") {
    require_fields(c, {"request_id", "action", "policy", "risk_artifact"});
    *result.mutable_live_policy()->mutable_policy() = encode_live_policy(c.at("policy"));
    result.mutable_live_policy()->set_risk_artifact(c.at("risk_artifact").get<std::string>());
  } else if (action == "live_resolve") {
    require_fields(c, {"request_id", "action", "order_id"});
    result.mutable_live_resolve()->set_order_id(c.at("order_id").get<std::string>());
  } else if (action == "strategy_start") {
    require_fields(c, {"request_id", "action", "venue", "symbol", "strategy", "market_endpoint",
                       "market_service"});
    auto* start = result.mutable_strategy_start();
    start->set_venue(c.at("venue").get<std::string>());
    start->set_symbol(c.at("symbol").get<std::string>());
    InstrumentId{start->venue(), start->symbol()}.validate();
    *start->mutable_strategy() = encode_strategy(c.at("strategy"));
    start->set_market_endpoint(c.at("market_endpoint").get<std::string>());
    start->set_market_service(c.at("market_service").get<std::string>());
  } else if (action == "strategy_stop") {
    require_fields(c, {"request_id", "action"});
    result.mutable_strategy_stop();
  } else
    throw std::invalid_argument("unsupported trading operation");
  return result;
}
Json decode_command(const v1::Command& c) {
  Json result{{"request_id", c.request_id()}};
  switch (c.operation_case()) {
  case v1::Command::kCancel:
    result["action"] = "cancel";
    result["order_id"] = c.cancel().order_id();
    break;
  case v1::Command::kSubmit: {
    const auto& o = c.submit();
    auto fields = instrument_fields(o.venue(), o.symbol());
    result.update({{"action", "submit"},
                   {"order_id", o.order_id()},
                   {"side", side(o.side())},
                   {"offset", offset(o.offset())},
                   {"quantity", get(o.quantity())},
                   {"price", get(o.price())}});
    result.update(fields);
    break;
  }
  case v1::Command::kLiveAuthorize:
    result.update({{"action", "live_authorize"}, {"user_id", c.live_authorize().user_id()}});
    break;
  case v1::Command::kLiveRevoke:
    result["action"] = "live_revoke";
    break;
  case v1::Command::kLivePolicy:
    result.update({{"action", "live_policy"},
                   {"policy", decode_live_policy(c.live_policy().policy())},
                   {"risk_artifact", c.live_policy().risk_artifact()}});
    break;
  case v1::Command::kLiveResolve:
    result.update({{"action", "live_resolve"}, {"order_id", c.live_resolve().order_id()}});
    break;
  case v1::Command::kStrategyStart: {
    const auto& start = c.strategy_start();
    result.update({{"action", "strategy_start"},
                   {"strategy", decode_strategy(start.strategy())},
                   {"market_endpoint", start.market_endpoint()},
                   {"market_service", start.market_service()}});
    result.update(instrument_fields(start.venue(), start.symbol()));
    break;
  }
  case v1::Command::kStrategyStop:
    result["action"] = "strategy_stop";
    break;
  default:
    throw std::invalid_argument("missing trading operation");
  }
  return result;
}
v1::Snapshot encode_snapshot(const Json& s) {
  v1::Snapshot result;
  *result.mutable_risk() = encode_risk(s.at("risk"));
  for (const auto& c : s.at("contracts")) {
    auto* item = result.add_contracts();
    *item->mutable_contract() = contract(c.at("contract"));
    *item->mutable_costs() = encode_costs(c.at("costs"));
    *item->mutable_cost_schedule() = encode_cost_schedule(c.at("cost_schedule"));
    set(item->mutable_mark(), c.at("mark"));
  }
#define VALUE(name) set(result.mutable_##name(), s.at(#name))
  VALUE(balance);
  VALUE(equity);
  VALUE(available);
  VALUE(margin);
  VALUE(frozen);
  VALUE(fees);
  VALUE(realized);
  VALUE(unrealized);
#undef VALUE
  result.set_cursor(s.at("cursor").get<std::uint32_t>());
  result.set_total(s.at("total").get<std::uint32_t>());
  if (!s.at("timestamp_ns").is_null())
    result.set_timestamp_ns(std::stoll(s.at("timestamp_ns").get<std::string>()));
  result.set_recovery_required(s.at("storage_state") == "recovery_required");
  for (const auto& p : s.at("positions")) {
    auto* item = result.add_positions();
    item->set_venue(p.at("venue").get<std::string>());
    item->set_symbol(p.at("symbol").get<std::string>());
    item->set_side(side(p.at("side").get<std::string>()));
    item->set_today(p.at("bucket") == "today");
    set(item->mutable_quantity(), p.at("quantity"));
    set(item->mutable_basis(), p.at("basis"));
  }
  for (const auto& o : s.at("orders")) {
    auto* item = result.add_orders();
    item->set_id(o.at("id").get<std::string>());
    item->set_venue(o.at("venue").get<std::string>());
    item->set_symbol(o.at("symbol").get<std::string>());
    item->set_side(side(o.at("side").get<std::string>()));
    item->set_offset(offset(o.at("offset").get<std::string>()));
    set(item->mutable_quantity(), o.at("quantity"));
    set(item->mutable_limit_price(), o.at("limit_price"));
    set(item->mutable_filled(), o.at("filled"));
    item->set_state(state(o.at("state").get<std::string>()));
  }
  for (const auto& f : s.at("fills")) {
    auto* item = result.add_fills();
    item->set_id(f.at("id").get<std::string>());
    item->set_order_id(f.at("order_id").get<std::string>());
    item->set_venue(f.at("venue").get<std::string>());
    item->set_symbol(f.at("symbol").get<std::string>());
    set(item->mutable_quantity(), f.at("quantity"));
    set(item->mutable_price(), f.at("price"));
  }
  return result;
}
Json decode_snapshot(const v1::Snapshot& s) {
  if (s.contracts().empty() ||
      static_cast<std::size_t>(s.contracts_size()) > max_portfolio_contracts || !s.has_balance() ||
      !s.has_equity() || !s.has_available() || !s.has_margin() || !s.has_frozen() ||
      !s.has_fees() || !s.has_realized() || !s.has_unrealized() || s.total() == 0 ||
      s.total() > max_dataset_bars || s.cursor() > s.total() ||
      (s.cursor() == 0) == s.has_timestamp_ns())
    throw std::invalid_argument("incomplete trading snapshot");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto risk = decode_risk(s.risk());
  Json contracts = Json::array();
  for (const auto& c : s.contracts()) {
    if (!c.has_contract() || !c.has_costs() || !c.has_mark())
      throw std::invalid_argument("incomplete trading snapshot");
    contracts.push_back({{"contract", contract(c.contract())},
                         {"costs", decode_costs(c.costs())},
                         {"cost_schedule", decode_cost_schedule(c.cost_schedule())},
                         {"mark", get(c.mark())}});
  }
  Json result{{"risk", std::move(risk)},
              {"contracts", std::move(contracts)},
              {"mode", "historical_paper"},
              {"persistent", true},
              {"storage_state", s.recovery_required() ? "recovery_required" : "ready"},
              {"cursor", s.cursor()},
              {"total", s.total()},
              {"timestamp_ns",
               s.has_timestamp_ns() ? Json(std::to_string(s.timestamp_ns())) : Json(nullptr)}};
#define VALUE(name) result[#name] = get(s.name())
  VALUE(balance);
  VALUE(equity);
  VALUE(available);
  VALUE(margin);
  VALUE(frozen);
  VALUE(fees);
  VALUE(realized);
  VALUE(unrealized);
#undef VALUE
  result["positions"] = Json::array();
  result["orders"] = Json::array();
  result["fills"] = Json::array();
  // Enum decoding rejects unknown values; it runs before each braced
  // initializer (GCC < 13 PR66139 leak).
  for (const auto& p : s.positions()) {
    auto item = instrument_fields(p.venue(), p.symbol());
    item.update({{"side", side(p.side())},
                 {"bucket", p.today() ? "today" : "yesterday"},
                 {"quantity", get(p.quantity())},
                 {"basis", get(p.basis())}});
    result["positions"].push_back(std::move(item));
  }
  for (const auto& o : s.orders()) {
    auto item = instrument_fields(o.venue(), o.symbol());
    item.update({{"id", o.id()},
                 {"side", side(o.side())},
                 {"offset", offset(o.offset())},
                 {"quantity", get(o.quantity())},
                 {"limit_price", get(o.limit_price())},
                 {"filled", get(o.filled())},
                 {"state", state(o.state())}});
    result["orders"].push_back(std::move(item));
  }
  for (const auto& f : s.fills()) {
    auto item = instrument_fields(f.venue(), f.symbol());
    item.update({{"id", f.id()},
                 {"order_id", f.order_id()},
                 {"quantity", get(f.quantity())},
                 {"price", get(f.price())}});
    result["fills"].push_back(std::move(item));
  }
  return result;
}
namespace {
// CTP identities: short printable text without spaces.
std::string broker_text(const Json& value, const char* name, std::size_t limit) {
  const auto text = value.at(name).get<std::string>();
  if (text.empty() || text.size() > limit ||
      std::ranges::any_of(text, [](unsigned char c) { return c <= ' ' || c > '~'; }))
    throw std::invalid_argument(std::string("invalid live broker ") + name);
  return text;
}
Json live_broker(const Json& b) {
  require_fields(b, {"front", "broker_id", "user_id", "app_id"});
  const auto front = broker_text(b, "front", 64);
  const auto colon = front.rfind(':');
  const auto host = front.substr(0, colon);
  const auto port = colon == std::string::npos ? std::string{} : front.substr(colon + 1);
  int number = 0;
  const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
  if (!front.starts_with("tcp://") || host.size() <= 6 || error != std::errc{} ||
      end != port.data() + port.size() || number < 1 || number > 65535 ||
      std::ranges::any_of(host.substr(6),
                          [](unsigned char c) { return !std::isalnum(c) && c != '.' && c != '-'; }))
    throw std::invalid_argument("live broker front must look like tcp://host:port");
  return {{"front", front},
          {"broker_id", broker_text(b, "broker_id", 10)},
          {"user_id", broker_text(b, "user_id", 15)},
          {"app_id", broker_text(b, "app_id", 32)}};
}
std::string live_costs_state(const std::string& value) {
  if (value != "querying" && value != "ready" && value != "unavailable")
    throw std::invalid_argument("invalid live costs state");
  return value;
}
std::string live_status(const std::string& value) {
  static const std::set<std::string> known{"submitted", "accepted",  "partially_filled",
                                           "filled",    "cancelled", "rejected"};
  if (!known.contains(value))
    throw std::invalid_argument("invalid live order status");
  return value;
}
} // namespace
v1::LivePolicy encode_live_policy(const Json& policy) {
  require_fields(policy, {"risk", "max_price_deviation", "contracts"});
  if (!policy.at("contracts").is_array())
    throw std::invalid_argument("live policy contracts must be an array");
  v1::LivePolicy result;
  *result.mutable_risk() = encode_risk(policy.at("risk"));
  set(result.mutable_max_price_deviation(), policy.at("max_price_deviation"));
  if (get(result.max_price_deviation()) != policy.at("max_price_deviation").get<std::string>())
    throw std::invalid_argument("price deviation requires canonical decimal text");
  for (const auto& c : policy.at("contracts"))
    *result.add_contracts() = contract(c);
  static_cast<void>(decode_live_policy(result));
  return result;
}
Json decode_live_policy(const v1::LivePolicy& policy) {
  if (!policy.has_risk() || policy.contracts().empty() ||
      static_cast<std::size_t>(policy.contracts_size()) > max_portfolio_contracts)
    throw std::invalid_argument("live policy requires risk and 1 to 20 contracts");
  auto risk = decode_risk(policy.risk());
  const auto deviation = Decimal::from_raw(policy.max_price_deviation().units());
  if (!policy.has_max_price_deviation() || deviation <= Decimal{} ||
      deviation >= Decimal::parse("1"))
    throw std::invalid_argument("price deviation limit must be above 0 and below 1");
  Json contracts = Json::array();
  std::set<InstrumentId> seen;
  for (const auto& c : policy.contracts()) {
    const auto terms = instrument(c);
    FuturesContract{terms, c.product(), c.delivery_month()}.validate();
    if (!seen.insert(terms.id).second)
      throw std::invalid_argument("duplicate live contract");
    contracts.push_back(contract(c));
  }
  return {{"risk", std::move(risk)},
          {"max_price_deviation", deviation.str()},
          {"contracts", std::move(contracts)}};
}
// Version 5 identifies the owned account record using its stable Terminal ID.
v1::LiveInput encode_live_input(const Json& m) {
  require_fields(m, {"version", "type", "account_id", "broker", "policy"});
  if (m.at("version") != 5 || m.at("type") != "live_ctp")
    throw std::invalid_argument("invalid live input");
  validate_id(m.at("account_id").get<std::string>());
  v1::LiveInput result;
  result.set_account_id(m.at("account_id"));
  const auto broker = live_broker(m.at("broker"));
  result.mutable_broker()->set_front(broker.at("front"));
  result.mutable_broker()->set_broker_id(broker.at("broker_id"));
  result.mutable_broker()->set_user_id(broker.at("user_id"));
  result.mutable_broker()->set_app_id(broker.at("app_id"));
  *result.mutable_policy() = encode_live_policy(m.at("policy"));
  return result;
}
Json decode_live_input(const v1::LiveInput& input) {
  if (!input.has_broker() || !input.has_policy())
    throw std::invalid_argument("live input requires a broker and policy");
  validate_id(input.account_id());
  auto broker = live_broker({{"front", input.broker().front()},
                             {"broker_id", input.broker().broker_id()},
                             {"user_id", input.broker().user_id()},
                             {"app_id", input.broker().app_id()}});
  auto policy = decode_live_policy(input.policy());
  return {{"version", 5},
          {"account_id", input.account_id()},
          {"type", "live_ctp"},
          {"broker", std::move(broker)},
          {"policy", std::move(policy)}};
}
v1::LiveSnapshot encode_live_snapshot(const Json& s) {
  v1::LiveSnapshot result;
  result.set_account_id(s.at("account_id").get<std::string>());
  result.set_segment_count(s.at("segment_count").get<std::uint64_t>());
  const auto& capacity = s.at("capacity");
  auto* stored = result.mutable_capacity();
  stored->set_records_used(capacity.at("records_used").get<std::uint64_t>());
  stored->set_records_limit(capacity.at("records_limit").get<std::uint64_t>());
  stored->set_bytes_used(capacity.at("bytes_used").get<std::uint64_t>());
  stored->set_bytes_limit(capacity.at("bytes_limit").get<std::uint64_t>());
  const auto input = encode_live_input({{"version", 5},
                                        {"account_id", s.at("account_id")},
                                        {"type", "live_ctp"},
                                        {"broker", s.at("broker")},
                                        {"policy",
                                         {{"risk", s.at("risk")},
                                          {"max_price_deviation", s.at("max_price_deviation")},
                                          {"contracts", s.at("contracts")}}}});
  *result.mutable_broker() = input.broker();
  *result.mutable_risk() = input.policy().risk();
  *result.mutable_max_price_deviation() = input.policy().max_price_deviation();
  *result.mutable_contracts() = input.policy().contracts();
  result.set_policy_revision(s.at("policy_revision").get<std::string>());
  result.set_risk_artifact(s.at("risk_artifact").get<std::string>());
  result.set_phase(s.at("phase").get<std::string>());
  result.set_error_code(s.at("error_code").get<int>());
  result.set_trading_day(s.at("trading_day").get<std::string>());
  result.set_synchronized_ms(s.at("synchronized_ms").get<std::int64_t>());
  if (!s.at("funds").is_null()) {
    const auto& f = s.at("funds");
    auto* funds = result.mutable_funds();
    set(funds->mutable_balance(), f.at("balance"));
    set(funds->mutable_available(), f.at("available"));
    set(funds->mutable_margin(), f.at("margin"));
    set(funds->mutable_commission(), f.at("commission"));
    set(funds->mutable_close_profit(), f.at("close_profit"));
    set(funds->mutable_position_profit(), f.at("position_profit"));
  }
  for (const auto& p : s.at("positions")) {
    auto* item = result.add_positions();
    item->set_venue(p.at("venue"));
    item->set_symbol(p.at("symbol"));
    item->set_side(side(p.at("side").get<std::string>()));
    set(item->mutable_today(), p.at("today"));
    set(item->mutable_yesterday(), p.at("yesterday"));
  }
  for (const auto& o : s.at("orders")) {
    auto* item = result.add_orders();
    item->set_id(o.at("id"));
    item->set_broker_key(o.at("broker_key"));
    item->set_exchange_order_id(o.at("exchange_order_id"));
    item->set_venue(o.at("venue"));
    item->set_symbol(o.at("symbol"));
    item->set_side(side(o.at("side").get<std::string>()));
    item->set_offset(offset(o.at("offset").get<std::string>()));
    set(item->mutable_quantity(), o.at("quantity"));
    set(item->mutable_filled(), o.at("filled"));
    set(item->mutable_limit_price(), o.at("limit_price"));
    item->set_status(live_status(o.at("status")));
    item->set_error_code(o.at("error_code").get<int>());
  }
  for (const auto& t : s.at("trades")) {
    auto* item = result.add_trades();
    item->set_id(t.at("id"));
    item->set_order_id(t.at("order_id"));
    item->set_venue(t.at("venue"));
    item->set_symbol(t.at("symbol"));
    item->set_side(side(t.at("side").get<std::string>()));
    item->set_offset(offset(t.at("offset").get<std::string>()));
    set(item->mutable_quantity(), t.at("quantity"));
    set(item->mutable_price(), t.at("price"));
    item->set_trading_day(t.at("trading_day"));
    item->set_trade_time(t.at("trade_time"));
  }
  if (!s.at("authorization").is_null()) {
    const auto& a = s.at("authorization");
    result.mutable_authorization()->set_authorized_at_ms(
        a.at("authorized_at_ms").get<std::int64_t>());
  }
  if (!s.at("strategy").is_null()) {
    const auto& run = s.at("strategy");
    auto* item = result.mutable_strategy();
    item->set_id(run.at("id"));
    item->set_venue(run.at("venue"));
    item->set_symbol(run.at("symbol"));
    *item->mutable_strategy() = encode_strategy(run.at("strategy"));
    item->set_warmup(run.at("warmup"));
    item->set_state(run.at("state"));
    item->set_reason(run.at("reason"));
    item->set_started_ms(run.at("started_ms"));
    item->set_bars(run.at("bars"));
    item->set_bar_ms(run.at("bar_ms"));
    if (!run.at("target").is_null())
      set(item->mutable_target(), run.at("target"));
    for (const auto& order : run.at("orders"))
      item->add_orders(order);
  }
  for (const auto& u : s.at("unconfirmed")) {
    auto* item = result.add_unconfirmed();
    item->set_id(u.at("id"));
    item->set_broker_key(u.at("broker_key"));
    item->set_trading_day(u.at("trading_day"));
  }
  result.set_recovery_required(s.at("storage_state") == "recovery_required");
  for (const auto& c : s.at("costs")) {
    auto* item = result.add_costs();
    item->set_venue(c.at("venue"));
    item->set_symbol(c.at("symbol"));
    item->set_state(live_costs_state(c.at("state")));
    item->set_error_code(c.at("error_code").get<int>());
    item->set_queried_ms(c.at("queried_ms").get<std::int64_t>());
    if (c.at("state") == "ready")
      *item->mutable_costs() = encode_costs(c.at("costs"));
  }
  return result;
}
Json decode_live_snapshot(const v1::LiveSnapshot& s) {
  validate_id(s.account_id());
  validate_id(s.policy_revision());
  if (s.risk_artifact().size() != 64)
    throw std::invalid_argument("invalid risk plugin artifact");
  if (s.segment_count() > 9007199254740991ULL)
    throw std::invalid_argument("invalid trading journal segment count");
  const auto& capacity = s.capacity();
  if (!s.has_capacity() || !capacity.records_limit() || !capacity.bytes_limit() ||
      capacity.records_used() > capacity.records_limit() ||
      capacity.bytes_used() > capacity.bytes_limit() ||
      capacity.records_limit() > 9007199254740991ULL ||
      capacity.bytes_limit() > 9007199254740991ULL)
    throw std::invalid_argument("invalid journal capacity");
  auto input = decode_live_input([&] {
    v1::LiveInput value;
    value.set_account_id(s.account_id());
    *value.mutable_broker() = s.broker();
    *value.mutable_policy()->mutable_risk() = s.risk();
    *value.mutable_policy()->mutable_max_price_deviation() = s.max_price_deviation();
    *value.mutable_policy()->mutable_contracts() = s.contracts();
    return value;
  }());
  static const std::set<std::string> phases{"disconnected", "connecting", "authenticating",
                                            "logging_in",   "confirming", "synchronizing",
                                            "ready",        "error"};
  if (!phases.contains(s.phase()))
    throw std::invalid_argument("invalid live phase");
  Json result{{"mode", "live"},
              {"account_id", s.account_id()},
              {"policy_revision", s.policy_revision()},
              {"risk_artifact", s.risk_artifact()},
              {"segment_count", s.segment_count()},
              {"broker", input.at("broker")},
              {"risk", input.at("policy").at("risk")},
              {"max_price_deviation", input.at("policy").at("max_price_deviation")},
              {"contracts", input.at("policy").at("contracts")},
              {"phase", s.phase()},
              {"error_code", s.error_code()},
              {"trading_day", s.trading_day()},
              {"synchronized_ms", s.synchronized_ms()},
              {"funds", nullptr},
              {"positions", Json::array()},
              {"orders", Json::array()},
              {"trades", Json::array()},
              {"authorization", nullptr},
              {"strategy", nullptr},
              {"unconfirmed", Json::array()},
              {"costs", Json::array()},
              {"storage_state", s.recovery_required() ? "recovery_required" : "ready"},
              {"capacity",
               {{"records_used", capacity.records_used()},
                {"records_limit", capacity.records_limit()},
                {"bytes_used", capacity.bytes_used()},
                {"bytes_limit", capacity.bytes_limit()}}}};
  for (const auto& c : s.costs()) {
    auto item = instrument_fields(c.venue(), c.symbol());
    const auto state = live_costs_state(c.state());
    if ((state == "ready") != c.has_costs())
      throw std::invalid_argument("live costs require rates exactly when ready");
    item.update({{"state", state},
                 {"error_code", c.error_code()},
                 {"queried_ms", c.queried_ms()},
                 {"costs", c.has_costs() ? decode_costs(c.costs()) : Json(nullptr)}});
    result["costs"].push_back(std::move(item));
  }
  if (s.has_funds()) {
    const auto& f = s.funds();
    result["funds"] = {{"balance", get(f.balance())},
                       {"available", get(f.available())},
                       {"margin", get(f.margin())},
                       {"commission", get(f.commission())},
                       {"close_profit", get(f.close_profit())},
                       {"position_profit", get(f.position_profit())}};
  }
  for (const auto& p : s.positions()) {
    auto item = instrument_fields(p.venue(), p.symbol());
    item.update(
        {{"side", side(p.side())}, {"today", get(p.today())}, {"yesterday", get(p.yesterday())}});
    result["positions"].push_back(std::move(item));
  }
  for (const auto& o : s.orders()) {
    auto item = instrument_fields(o.venue(), o.symbol());
    item.update({{"id", o.id()},
                 {"broker_key", o.broker_key()},
                 {"exchange_order_id", o.exchange_order_id()},
                 {"side", side(o.side())},
                 {"offset", offset(o.offset())},
                 {"quantity", get(o.quantity())},
                 {"filled", get(o.filled())},
                 {"limit_price", get(o.limit_price())},
                 {"status", live_status(o.status())},
                 {"error_code", o.error_code()}});
    result["orders"].push_back(std::move(item));
  }
  for (const auto& t : s.trades()) {
    auto item = instrument_fields(t.venue(), t.symbol());
    item.update({{"id", t.id()},
                 {"order_id", t.order_id()},
                 {"side", side(t.side())},
                 {"offset", offset(t.offset())},
                 {"quantity", get(t.quantity())},
                 {"price", get(t.price())},
                 {"trading_day", t.trading_day()},
                 {"trade_time", t.trade_time()}});
    result["trades"].push_back(std::move(item));
  }
  if (s.has_authorization())
    result["authorization"] = {{"authorized_at_ms", s.authorization().authorized_at_ms()}};
  if (s.has_strategy()) {
    const auto& run = s.strategy();
    if (run.state() != "running" && run.state() != "stopped")
      throw std::invalid_argument("invalid strategy run state");
    auto item = instrument_fields(run.venue(), run.symbol());
    item.update({{"id", run.id()},
                 {"strategy", decode_strategy(run.strategy())},
                 {"warmup", run.warmup()},
                 {"state", run.state()},
                 {"reason", run.reason()},
                 {"started_ms", run.started_ms()},
                 {"bars", run.bars()},
                 {"bar_ms", run.bar_ms()},
                 {"target", run.has_target() ? Json(get(run.target())) : Json(nullptr)},
                 {"orders", Json::array()}});
    for (const auto& order : run.orders())
      item["orders"].push_back(order);
    result["strategy"] = std::move(item);
  }
  for (const auto& u : s.unconfirmed())
    result["unconfirmed"].push_back(
        {{"id", u.id()}, {"broker_key", u.broker_key()}, {"trading_day", u.trading_day()}});
  return result;
}
} // namespace asterion::protocol
