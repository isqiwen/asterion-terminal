#include <asterion/foundation/decimal.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/trading.hpp>
#include <google/protobuf/unknown_field_set.h>
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
  throw std::invalid_argument("invalid offset");
}
std::string offset(v1::Offset value) {
  if (value == v1::OPEN)
    return "open";
  if (value == v1::CLOSE_TODAY)
    return "close_today";
  if (value == v1::CLOSE_YESTERDAY)
    return "close_yesterday";
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
v1::Costs costs(const Json& c) {
  require_fields(c, {"margin_per_lot", "open_fee", "close_today_fee", "close_yesterday_fee"});
  v1::Costs result;
  set(result.mutable_margin_per_lot(), c.at("margin_per_lot"));
  set(result.mutable_open_fee(), c.at("open_fee"));
  set(result.mutable_close_today_fee(), c.at("close_today_fee"));
  set(result.mutable_close_yesterday_fee(), c.at("close_yesterday_fee"));
  return result;
}
Json costs(const v1::Costs& c) {
  return {{"margin_per_lot", get(c.margin_per_lot())},
          {"open_fee", get(c.open_fee())},
          {"close_today_fee", get(c.close_today_fee())},
          {"close_yesterday_fee", get(c.close_yesterday_fee())}};
}
} // namespace
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
v1::Tick encode_tick(const Json& row) {
  require_fields(row, {"timestamp_ns", "price", "quantity"});
  v1::Tick tick;
  const auto time = row.at("timestamp_ns").get<std::string>();
  std::int64_t ns = 0;
  auto [end, error] = std::from_chars(time.data(), time.data() + time.size(), ns);
  if (error != std::errc{} || end != time.data() + time.size() || std::to_string(ns) != time)
    throw std::invalid_argument("invalid timestamp");
  tick.set_timestamp_ns(ns);
  set(tick.mutable_price(), row.at("price"));
  set(tick.mutable_quantity(), row.at("quantity"));
  return tick;
}
Json decode_tick(const v1::Tick& tick) {
  return {{"timestamp_ns", std::to_string(tick.timestamp_ns())},
          {"price", get(tick.price())},
          {"quantity", get(tick.quantity())}};
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
v1::PaperInput encode_input(const Json& m) {
  require_fields(m, {"version", "type", "contract", "costs", "deposit", "ticks", "risk"});
  if (m.at("version") != 1 || m.at("type") != "historical_paper")
    throw std::invalid_argument("invalid paper input");
  v1::PaperInput result;
  *result.mutable_risk() = encode_risk(m.at("risk"));
  *result.mutable_contract() = contract(m.at("contract"));
  *result.mutable_costs() = costs(m.at("costs"));
  set(result.mutable_deposit(), m.at("deposit"));
  if (!m.at("ticks").is_array() || m.at("ticks").size() > 10000)
    throw std::invalid_argument("invalid tick count");
  for (const auto& row : m.at("ticks"))
    *result.add_ticks() = encode_tick(row);
  return result;
}
Json decode_input(const v1::PaperInput& input) {
  if (!input.has_costs() || !input.costs().has_open_fee() || !input.costs().has_close_today_fee() ||
      !input.costs().has_close_yesterday_fee())
    throw std::invalid_argument("missing explicit paper fees");
  Json rows = Json::array();
  for (const auto& t : input.ticks())
    rows.push_back({{"timestamp_ns", std::to_string(t.timestamp_ns())},
                    {"price", get(t.price())},
                    {"quantity", get(t.quantity())}});
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto risk = decode_risk(input.risk());
  return {{"version", 1},
          {"type", "historical_paper"},
          {"contract", contract(input.contract())},
          {"costs", costs(input.costs())},
          {"deposit", get(input.deposit())},
          {"ticks", std::move(rows)},
          {"risk", std::move(risk)}};
}
v1::Command encode_command(const Json& c) {
  v1::Command result;
  result.set_request_id(c.at("request_id").get<std::string>());
  const auto action = c.at("action").get<std::string>();
  if (action == "replay_calendar") {
    require_fields(c, {"request_id", "action", "publication"});
    *result.mutable_replay_calendar()->mutable_publication() =
        encode_calendar_publication(c.at("publication"));
  } else if (action == "replay_settle") {
    require_fields(c, {"request_id", "action", "day_index"});
    if (!c.at("day_index").is_number_integer() || c.at("day_index") < 0 || c.at("day_index") >= 64)
      throw std::invalid_argument("invalid settlement day index");
    result.mutable_replay_settle()->set_day_index(c.at("day_index").get<unsigned>());
  } else if (action == "advance") {
    require_fields(c, {"request_id", "action"});
    result.mutable_advance();
  } else if (action == "cancel") {
    require_fields(c, {"request_id", "action", "order_id"});
    result.mutable_cancel()->set_order_id(c.at("order_id").get<std::string>());
  } else if (action == "settle") {
    require_fields(c, {"request_id", "action", "price"});
    set(result.mutable_settle()->mutable_price(), c.at("price"));
  } else if (action == "submit") {
    require_fields(c, {"request_id", "action", "order_id", "side", "offset", "quantity", "price"});
    auto* order = result.mutable_submit();
    order->set_order_id(c.at("order_id").get<std::string>());
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
  } else if (action == "strategy_revoke") {
    require_fields(c, {"request_id", "action", "grant_id"});
    result.mutable_strategy_revoke()->set_grant_id(c.at("grant_id").get<std::string>());
  } else if (action == "strategy_target") {
    require_fields(c, {"request_id", "action", "grant_id", "strategy_id", "stream_id",
                       "dataset_revision", "sequence", "timestamp_ns", "target_quantity"});
    if (!c.at("sequence").is_number_integer() || c.at("sequence") < 1 || c.at("sequence") > 10000)
      throw std::invalid_argument("invalid strategy sequence");
    auto* t = result.mutable_strategy_target();
    t->set_grant_id(c.at("grant_id").get<std::string>());
    t->set_strategy_id(c.at("strategy_id").get<std::string>());
    t->set_stream_id(c.at("stream_id").get<std::string>());
    t->set_dataset_revision(c.at("dataset_revision").get<std::string>());
    t->set_sequence(c.at("sequence").get<std::uint64_t>());
    const auto tick =
        encode_tick({{"timestamp_ns", c.at("timestamp_ns")}, {"price", "0"}, {"quantity", "1"}});
    t->set_timestamp_ns(tick.timestamp_ns());
    set(t->mutable_target_quantity(), c.at("target_quantity"));
  } else
    throw std::invalid_argument("unsupported trading operation");
  return result;
}
Json decode_command(const v1::Command& c) {
  Json result{{"request_id", c.request_id()}};
  switch (c.operation_case()) {
  case v1::Command::kReplayCalendar: {
    // Decoded before the braced initializer (GCC < 13 PR66139 leak).
    auto publication = decode_calendar_publication(c.replay_calendar().publication());
    result.update({{"action", "replay_calendar"}, {"publication", std::move(publication)}});
    break;
  }
  case v1::Command::kReplaySettle:
    if (c.replay_settle().day_index() >= 64)
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
  case v1::Command::kSettle:
    result["action"] = "settle";
    result["price"] = get(c.settle().price());
    break;
  case v1::Command::kSubmit:
    result["action"] = "submit";
    result["order_id"] = c.submit().order_id();
    result["side"] = side(c.submit().side());
    result["offset"] = offset(c.submit().offset());
    result["quantity"] = get(c.submit().quantity());
    result["price"] = get(c.submit().price());
    break;
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
  case v1::Command::kStrategyRevoke:
    result.update({{"action", "strategy_revoke"}, {"grant_id", c.strategy_revoke().grant_id()}});
    break;
  case v1::Command::kStrategyTarget: {
    const auto& t = c.strategy_target();
    if (!t.has_target_quantity() || !t.sequence() || t.sequence() > 10000)
      throw std::invalid_argument("incomplete strategy target");
    result.update({{"action", "strategy_target"},
                   {"grant_id", t.grant_id()},
                   {"strategy_id", t.strategy_id()},
                   {"stream_id", t.stream_id()},
                   {"dataset_revision", t.dataset_revision()},
                   {"sequence", t.sequence()},
                   {"timestamp_ns", std::to_string(t.timestamp_ns())},
                   {"target_quantity", get(t.target_quantity())}});
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
    *result.mutable_replay()->mutable_publication() =
        encode_calendar_publication(r.at("publication"));
    result.mutable_replay()->set_settled_days(r.at("settled_days").get<unsigned>());
    result.mutable_replay()->set_settlement_due(r.at("settlement_due").get<bool>());
    result.mutable_replay()->set_session_end(r.at("session_end").get<bool>());
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
  *result.mutable_contract() = contract(s.at("contract"));
  *result.mutable_costs() = costs(s.at("costs"));
#define VALUE(name) set(result.mutable_##name(), s.at(#name))
  VALUE(balance);
  VALUE(equity);
  VALUE(available);
  VALUE(margin);
  VALUE(frozen);
  VALUE(fees);
  VALUE(realized);
  VALUE(unrealized);
  VALUE(mark);
#undef VALUE
  result.set_cursor(s.at("cursor").get<std::uint32_t>());
  result.set_total(s.at("total").get<std::uint32_t>());
  if (!s.at("timestamp_ns").is_null())
    result.set_timestamp_ns(std::stoll(s.at("timestamp_ns").get<std::string>()));
  result.set_recovery_required(s.at("storage_state") == "recovery_required");
  for (const auto& p : s.at("positions")) {
    auto* item = result.add_positions();
    item->set_side(side(p.at("side").get<std::string>()));
    item->set_today(p.at("bucket") == "today");
    set(item->mutable_quantity(), p.at("quantity"));
    set(item->mutable_basis(), p.at("basis"));
  }
  for (const auto& o : s.at("orders")) {
    auto* item = result.add_orders();
    item->set_id(o.at("id").get<std::string>());
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
    set(item->mutable_quantity(), f.at("quantity"));
    set(item->mutable_price(), f.at("price"));
  }
  return result;
}
Json decode_snapshot(const v1::Snapshot& s) {
  if (!s.has_contract() || !s.has_costs() || !s.has_balance() || !s.has_equity() ||
      !s.has_available() || !s.has_margin() || !s.has_frozen() || !s.has_fees() ||
      !s.has_realized() || !s.has_unrealized() || !s.has_mark() || s.total() == 0 ||
      s.total() > 10000 || s.cursor() > s.total() || (s.cursor() == 0) == s.has_timestamp_ns())
    throw std::invalid_argument("incomplete trading snapshot");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto risk = decode_risk(s.risk());
  Json result{{"risk", std::move(risk)},
              {"contract", contract(s.contract())},
              {"costs", costs(s.costs())},
              {"mode", "historical_paper"},
              {"persistent", true},
              {"storage_state", s.recovery_required() ? "recovery_required" : "ready"},
              {"cursor", s.cursor()},
              {"total", s.total()},
              {"timestamp_ns",
               s.has_timestamp_ns() ? Json(std::to_string(s.timestamp_ns())) : Json(nullptr)}};
  if (s.has_replay()) {
    auto publication = decode_calendar_publication(s.replay().publication());
    if (s.replay().settled_days() > publication.at("calendar").at("days").size())
      throw std::invalid_argument("invalid settled day count");
    result["replay"] = {{"publication", publication},
                        {"settled_days", s.replay().settled_days()},
                        {"settlement_due", s.replay().settlement_due()},
                        {"session_end", s.replay().session_end()}};
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
  VALUE(mark);
#undef VALUE
  result["positions"] = Json::array();
  result["orders"] = Json::array();
  result["fills"] = Json::array();
  // Enum decoding rejects unknown values; it runs before each braced
  // initializer (GCC < 13 PR66139 leak).
  for (const auto& p : s.positions()) {
    const auto position_side = side(p.side());
    result["positions"].push_back({{"side", position_side},
                                   {"bucket", p.today() ? "today" : "yesterday"},
                                   {"quantity", get(p.quantity())},
                                   {"basis", get(p.basis())}});
  }
  for (const auto& o : s.orders()) {
    const auto order_side = side(o.side());
    const auto order_offset = offset(o.offset());
    const auto order_state = state(o.state());
    result["orders"].push_back({{"id", o.id()},
                                {"side", order_side},
                                {"offset", order_offset},
                                {"quantity", get(o.quantity())},
                                {"limit_price", get(o.limit_price())},
                                {"filled", get(o.filled())},
                                {"state", order_state}});
  }
  for (const auto& f : s.fills())
    result["fills"].push_back({{"id", f.id()},
                               {"order_id", f.order_id()},
                               {"quantity", get(f.quantity())},
                               {"price", get(f.price())}});
  return result;
}
} // namespace asterion::protocol
