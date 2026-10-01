#include <asterion/foundation/decimal.hpp>
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
void validate_message(const google::protobuf::Message& message) {
  const auto* reflection = message.GetReflection();
  if (reflection->GetUnknownFields(message).field_count())
    throw std::invalid_argument("unknown Protobuf field");
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(message, &fields);
  for (const auto* field : fields)
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      if (field->is_repeated()) {
        for (int i = 0; i < reflection->FieldSize(message, field); ++i)
          validate_message(reflection->GetRepeatedMessage(message, field, i));
      } else
        validate_message(reflection->GetMessage(message, field));
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
constexpr unsigned max_events = 20000;
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
  return {instrument(contract.dataset().contract()), futures_costs(contract.costs())};
}
// Manifest version 3: a portfolio of contracts, each with its dataset and costs.
v1::PaperInput encode_input(const Json& m) {
  require_fields(m, {"version", "type", "deposit", "risk", "contracts"});
  if (m.at("version") != 3 || m.at("type") != "historical_paper" || !m.at("contracts").is_array() ||
      m.at("contracts").empty() || m.at("contracts").size() > max_portfolio_contracts)
    throw std::invalid_argument("invalid paper input");
  v1::PaperInput result;
  set(result.mutable_deposit(), m.at("deposit"));
  *result.mutable_risk() = encode_risk(m.at("risk"));
  for (const auto& c : m.at("contracts")) {
    require_fields(c, {"dataset", "costs"});
    auto* contract = result.add_contracts();
    *contract->mutable_dataset() = encode_bar_dataset(c.at("dataset"));
    *contract->mutable_costs() = encode_costs(c.at("costs"));
  }
  static_cast<void>(decode_input(result));
  return result;
}
Json decode_input(const v1::PaperInput& input) {
  if (!input.has_deposit() || !input.has_risk() || input.contracts().empty() ||
      static_cast<std::size_t>(input.contracts_size()) > max_portfolio_contracts)
    throw std::invalid_argument("paper input requires deposit, risk and 1 to 20 contracts");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto risk = decode_risk(input.risk());
  Json contracts = Json::array();
  std::vector<ContractTerms> terms;
  for (const auto& c : input.contracts()) {
    if (!c.has_dataset() || !c.has_costs())
      throw std::invalid_argument("missing explicit paper costs or dataset");
    contracts.push_back(
        {{"dataset", decode_bar_dataset(c.dataset())}, {"costs", decode_costs(c.costs())}});
    terms.push_back(contract_terms(c));
  }
  // The account validates currency, uniqueness and each contract's terms.
  static_cast<void>(FuturesAccount(Decimal::from_raw(input.deposit().units()), terms));
  return {{"version", 3},
          {"type", "historical_paper"},
          {"deposit", get(input.deposit())},
          {"risk", std::move(risk)},
          {"contracts", std::move(contracts)}};
}
v1::Command encode_command(const Json& c) {
  v1::Command result;
  result.set_request_id(c.at("request_id").get<std::string>());
  const auto action = c.at("action").get<std::string>();
  if (action == "replay_settle") {
    require_fields(c, {"request_id", "action", "day_index"});
    if (!c.at("day_index").is_number_integer() || c.at("day_index") < 0 ||
        c.at("day_index") >= max_events)
      throw std::invalid_argument("invalid settlement day index");
    result.mutable_replay_settle()->set_day_index(c.at("day_index").get<unsigned>());
  } else if (action == "advance") {
    require_fields(c, {"request_id", "action"});
    result.mutable_advance();
  } else if (action == "cancel") {
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
  } else if (action == "strategy_grant") {
    require_fields(c, {"request_id", "action", "grant_id", "strategy_id", "stream_id",
                       "dataset_revision", "max_quantity"});
    auto* g = result.mutable_strategy_grant();
    g->set_grant_id(c.at("grant_id").get<std::string>());
    g->set_strategy_id(c.at("strategy_id").get<std::string>());
    g->set_stream_id(c.at("stream_id").get<std::string>());
    g->set_dataset_revision(c.at("dataset_revision").get<std::string>());
    set(g->mutable_max_quantity(), c.at("max_quantity"));
  } else if (action == "live_authorize") {
    require_fields(c, {"request_id", "action", "user_id"});
    result.mutable_live_authorize()->set_user_id(c.at("user_id").get<std::string>());
  } else if (action == "live_revoke") {
    require_fields(c, {"request_id", "action"});
    result.mutable_live_revoke();
  } else if (action == "strategy_revoke") {
    require_fields(c, {"request_id", "action", "grant_id"});
    result.mutable_strategy_revoke()->set_grant_id(c.at("grant_id").get<std::string>());
  } else if (action == "strategy_target") {
    require_fields(c, {"request_id", "action", "grant_id", "strategy_id", "stream_id",
                       "dataset_revision", "sequence", "timestamp_ns", "venue", "symbol",
                       "target_quantity"});
    if (!c.at("sequence").is_number_integer() || c.at("sequence") < 1 ||
        c.at("sequence") > max_events)
      throw std::invalid_argument("invalid strategy sequence");
    auto* t = result.mutable_strategy_target();
    t->set_grant_id(c.at("grant_id").get<std::string>());
    t->set_strategy_id(c.at("strategy_id").get<std::string>());
    t->set_stream_id(c.at("stream_id").get<std::string>());
    t->set_dataset_revision(c.at("dataset_revision").get<std::string>());
    t->set_sequence(c.at("sequence").get<std::uint64_t>());
    const auto time = c.at("timestamp_ns").get<std::string>();
    std::int64_t ns = 0;
    const auto [end, error] = std::from_chars(time.data(), time.data() + time.size(), ns);
    if (error != std::errc{} || end != time.data() + time.size() || std::to_string(ns) != time)
      throw std::invalid_argument("invalid timestamp");
    t->set_timestamp_ns(ns);
    t->set_venue(c.at("venue").get<std::string>());
    t->set_symbol(c.at("symbol").get<std::string>());
    InstrumentId{t->venue(), t->symbol()}.validate();
    set(t->mutable_target_quantity(), c.at("target_quantity"));
  } else
    throw std::invalid_argument("unsupported trading operation");
  return result;
}
Json decode_command(const v1::Command& c) {
  Json result{{"request_id", c.request_id()}};
  switch (c.operation_case()) {
  case v1::Command::kReplaySettle:
    if (c.replay_settle().day_index() >= max_events)
      throw std::invalid_argument("invalid settlement day index");
    result.update({{"action", "replay_settle"}, {"day_index", c.replay_settle().day_index()}});
    break;
  case v1::Command::kAdvance:
    result["action"] = "advance";
    break;
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
  case v1::Command::kStrategyGrant: {
    const auto& g = c.strategy_grant();
    if (!g.has_max_quantity())
      throw std::invalid_argument("missing strategy position limit");
    result.update({{"action", "strategy_grant"},
                   {"grant_id", g.grant_id()},
                   {"strategy_id", g.strategy_id()},
                   {"stream_id", g.stream_id()},
                   {"dataset_revision", g.dataset_revision()},
                   {"max_quantity", get(g.max_quantity())}});
    break;
  }
  case v1::Command::kLiveAuthorize:
    result.update({{"action", "live_authorize"}, {"user_id", c.live_authorize().user_id()}});
    break;
  case v1::Command::kLiveRevoke:
    result["action"] = "live_revoke";
    break;
  case v1::Command::kStrategyRevoke:
    result.update({{"action", "strategy_revoke"}, {"grant_id", c.strategy_revoke().grant_id()}});
    break;
  case v1::Command::kStrategyTarget: {
    const auto& t = c.strategy_target();
    if (!t.has_target_quantity() || !t.sequence() || t.sequence() > max_events)
      throw std::invalid_argument("incomplete strategy target");
    auto fields = instrument_fields(t.venue(), t.symbol());
    result.update({{"action", "strategy_target"},
                   {"grant_id", t.grant_id()},
                   {"strategy_id", t.strategy_id()},
                   {"stream_id", t.stream_id()},
                   {"dataset_revision", t.dataset_revision()},
                   {"sequence", t.sequence()},
                   {"timestamp_ns", std::to_string(t.timestamp_ns())},
                   {"target_quantity", get(t.target_quantity())}});
    result.update(fields);
    break;
  }
  default:
    throw std::invalid_argument("missing trading operation");
  }
  return result;
}
v1::Snapshot encode_snapshot(const Json& s) {
  v1::Snapshot result;
  if (s.contains("replay")) {
    const auto& r = s.at("replay");
    result.mutable_replay()->set_settled_days(r.at("settled_days").get<unsigned>());
    result.mutable_replay()->set_settlement_due(r.at("settlement_due").get<bool>());
    result.mutable_replay()->set_day_end(r.at("day_end").get<bool>());
    result.mutable_replay()->set_days(r.at("days").get<unsigned>());
  }
  if (s.contains("strategy")) {
    const auto& g = s.at("strategy");
    auto command = g;
    command.erase("active");
    command.erase("last_sequence");
    command["action"] = "strategy_grant";
    command["request_id"] = "snapshot";
    *result.mutable_strategy()->mutable_grant() = encode_command(command).strategy_grant();
    result.mutable_strategy()->set_active(g.at("active").get<bool>());
    result.mutable_strategy()->set_last_sequence(g.at("last_sequence").get<std::uint64_t>());
  }
  *result.mutable_risk() = encode_risk(s.at("risk"));
  for (const auto& c : s.at("contracts")) {
    auto* item = result.add_contracts();
    *item->mutable_contract() = contract(c.at("contract"));
    *item->mutable_costs() = encode_costs(c.at("costs"));
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
      s.total() > max_events || s.cursor() > s.total() || (s.cursor() == 0) == s.has_timestamp_ns())
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
  if (s.has_replay()) {
    if (s.replay().settled_days() > s.replay().days() || s.replay().days() > s.total())
      throw std::invalid_argument("invalid settled day count");
    result["replay"] = {{"settled_days", s.replay().settled_days()},
                        {"settlement_due", s.replay().settlement_due()},
                        {"day_end", s.replay().day_end()},
                        {"days", s.replay().days()}};
  }
  if (s.has_strategy()) {
    if (!s.strategy().has_grant() || s.strategy().last_sequence() > s.cursor())
      throw std::invalid_argument("invalid strategy authorization snapshot");
    v1::Command command;
    *command.mutable_strategy_grant() = s.strategy().grant();
    auto grant = decode_command(command);
    grant.erase("request_id");
    grant.erase("action");
    grant["active"] = s.strategy().active();
    grant["last_sequence"] = s.strategy().last_sequence();
    result["strategy"] = std::move(grant);
  }
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
// Live manifest version 1: a CTP account, its risk limits and the contracts
// it may trade.
v1::LiveInput encode_live_input(const Json& m) {
  require_fields(m, {"version", "type", "broker", "risk", "contracts"});
  if (m.at("version") != 1 || m.at("type") != "live_ctp" || !m.at("contracts").is_array())
    throw std::invalid_argument("invalid live input");
  v1::LiveInput result;
  const auto broker = live_broker(m.at("broker"));
  result.mutable_broker()->set_front(broker.at("front"));
  result.mutable_broker()->set_broker_id(broker.at("broker_id"));
  result.mutable_broker()->set_user_id(broker.at("user_id"));
  result.mutable_broker()->set_app_id(broker.at("app_id"));
  *result.mutable_risk() = encode_risk(m.at("risk"));
  for (const auto& c : m.at("contracts"))
    *result.add_contracts() = contract(c);
  static_cast<void>(decode_live_input(result));
  return result;
}
Json decode_live_input(const v1::LiveInput& input) {
  if (!input.has_broker() || !input.has_risk() || input.contracts().empty() ||
      static_cast<std::size_t>(input.contracts_size()) > max_portfolio_contracts)
    throw std::invalid_argument("live input requires a broker, risk and 1 to 20 contracts");
  auto broker = live_broker({{"front", input.broker().front()},
                             {"broker_id", input.broker().broker_id()},
                             {"user_id", input.broker().user_id()},
                             {"app_id", input.broker().app_id()}});
  auto risk = decode_risk(input.risk());
  Json contracts = Json::array();
  std::set<InstrumentId> seen;
  for (const auto& c : input.contracts()) {
    const auto terms = instrument(c);
    terms.validate();
    if (!seen.insert(terms.id).second)
      throw std::invalid_argument("duplicate live contract");
    contracts.push_back(contract(c));
  }
  return {{"version", 1},
          {"type", "live_ctp"},
          {"broker", std::move(broker)},
          {"risk", std::move(risk)},
          {"contracts", std::move(contracts)}};
}
v1::LiveSnapshot encode_live_snapshot(const Json& s) {
  v1::LiveSnapshot result;
  const auto input = encode_live_input({{"version", 1},
                                        {"type", "live_ctp"},
                                        {"broker", s.at("broker")},
                                        {"risk", s.at("risk")},
                                        {"contracts", s.at("contracts")}});
  *result.mutable_broker() = input.broker();
  *result.mutable_risk() = input.risk();
  *result.mutable_contracts() = input.contracts();
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
    result.mutable_authorization()->set_trading_day(a.at("trading_day"));
    result.mutable_authorization()->set_authorized_at_ms(
        a.at("authorized_at_ms").get<std::int64_t>());
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
  auto input = decode_live_input([&] {
    v1::LiveInput value;
    *value.mutable_broker() = s.broker();
    *value.mutable_risk() = s.risk();
    *value.mutable_contracts() = s.contracts();
    return value;
  }());
  static const std::set<std::string> phases{"disconnected", "connecting", "authenticating",
                                            "logging_in",   "confirming", "synchronizing",
                                            "ready",        "error"};
  if (!phases.contains(s.phase()))
    throw std::invalid_argument("invalid live phase");
  Json result{{"mode", "live"},
              {"broker", input.at("broker")},
              {"risk", input.at("risk")},
              {"contracts", input.at("contracts")},
              {"phase", s.phase()},
              {"error_code", s.error_code()},
              {"trading_day", s.trading_day()},
              {"synchronized_ms", s.synchronized_ms()},
              {"funds", nullptr},
              {"positions", Json::array()},
              {"orders", Json::array()},
              {"trades", Json::array()},
              {"authorization", nullptr},
              {"unconfirmed", Json::array()},
              {"costs", Json::array()},
              {"storage_state", s.recovery_required() ? "recovery_required" : "ready"}};
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
    result["authorization"] = {{"trading_day", s.authorization().trading_day()},
                               {"authorized_at_ms", s.authorization().authorized_at_ms()}};
  for (const auto& u : s.unconfirmed())
    result["unconfirmed"].push_back(
        {{"id", u.id()}, {"broker_key", u.broker_key()}, {"trading_day", u.trading_day()}});
  return result;
}
} // namespace asterion::protocol
