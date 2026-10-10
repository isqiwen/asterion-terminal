#include "account_records.hpp"
#include <asterion/protocol/trading.hpp>
#include <stdexcept>
namespace asterion::trading {
namespace {
std::string text(const Json& value, const char* key) {
  auto result = value.at(key).get<std::string>();
  if (result.empty() || result.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid live trading field");
  return result;
}
Side side_of(const std::string& value) {
  if (value == "buy")
    return Side::buy;
  if (value == "sell")
    return Side::sell;
  throw std::invalid_argument("invalid order side");
}
Offset offset_of(const std::string& value) {
  if (value == "open")
    return Offset::open;
  if (value == "close_today")
    return Offset::close_today;
  if (value == "close_yesterday")
    return Offset::close_yesterday;
  if (value == "close")
    return Offset::close;
  throw std::invalid_argument("invalid open/close offset");
}
bool not_sent_code(int code) {
  return code == broker_code::session_changed || code == broker_code::exposure_changed ||
         code == broker_code::expired || code == broker_code::dispatch_busy ||
         code == broker_code::permit_refused;
}
TerminalOrder terminal_order(const Json& record) {
  require_fields(record, {"order_id", "broker_key", "trading_day", "status", "filled",
                          "exchange_order_id", "error_code"});
  const auto status = text(record, "status");
  if (!record.at("broker_key").is_string() || !record.at("trading_day").is_string())
    throw std::invalid_argument("invalid persisted terminal order evidence");
  TerminalOrder result{text(record, "order_id"),
                       record.at("broker_key").get<std::string>(),
                       record.at("trading_day").get<std::string>(),
                       status == "filled"      ? BrokerOrderStatus::filled
                       : status == "cancelled" ? BrokerOrderStatus::cancelled
                                               : BrokerOrderStatus::rejected,
                       Decimal::parse(text(record, "filled")),
                       {},
                       0};
  if ((status != "filled" && status != "cancelled" && status != "rejected") ||
      result.filled < Decimal{} ||
      (result.status == BrokerOrderStatus::rejected && result.filled != Decimal{}) ||
      !record.at("exchange_order_id").is_string() || !record.at("error_code").is_number_integer())
    throw std::invalid_argument("invalid persisted terminal order evidence");
  result.exchange_order_id = record.at("exchange_order_id").get<std::string>();
  result.error_code = record.at("error_code").get<int>();
  return result;
}
JournalEntry command_body(const AccountRequest& request, std::string_view policy_revision) {
  JournalEntry result;
  result.body = {{"command", request.command}, {"policy_revision", policy_revision}};
  result.command_id = request.id;
  result.trace = {
      request.id, std::string(request.action()), std::string(request.order_id()), {}, {}};
  return result;
}
} // namespace
std::string_view side_name(Side side) noexcept {
  return side == Side::buy ? "buy" : "sell";
}
std::string_view offset_name(Offset offset) noexcept {
  switch (offset) {
  case Offset::open:
    return "open";
  case Offset::close_today:
    return "close_today";
  case Offset::close_yesterday:
    return "close_yesterday";
  case Offset::close:
    return "close";
  }
  return "open";
}
std::string_view status_name(BrokerOrderStatus status) noexcept {
  switch (status) {
  case BrokerOrderStatus::submitted:
    return "submitted";
  case BrokerOrderStatus::accepted:
    return "accepted";
  case BrokerOrderStatus::partially_filled:
    return "partially_filled";
  case BrokerOrderStatus::filled:
    return "filled";
  case BrokerOrderStatus::cancelled:
    return "cancelled";
  case BrokerOrderStatus::rejected:
    return "rejected";
  }
  return "submitted";
}
AccountRequest AccountRequest::parse(const Json& command) {
  AccountRequest result;
  result.id = text(command, "request_id");
  validate_id(result.id);
  const auto action = text(command, "action");
  if (action == "submit") {
    require_fields(command, {"request_id", "action", "order_id", "venue", "symbol", "side",
                             "offset", "quantity", "price"});
    auto order_id = text(command, "order_id");
    validate_id(order_id);
    result.operation = SubmitOrder{{std::move(order_id),
                                    {text(command, "venue"), text(command, "symbol")},
                                    side_of(text(command, "side")),
                                    Decimal::parse(text(command, "quantity")),
                                    Decimal::parse(text(command, "price"))},
                                   offset_of(text(command, "offset"))};
  } else if (action == "cancel") {
    require_fields(command, {"request_id", "action", "order_id"});
    result.operation = CancelOrder{text(command, "order_id")};
  } else if (action == "live_authorize") {
    require_fields(command, {"request_id", "action", "user_id"});
    result.operation = Authorize{command.at("user_id").get<std::string>()};
  } else if (action == "live_revoke") {
    require_fields(command, {"request_id", "action"});
    result.operation = Revoke{};
  } else if (action == "live_resolve") {
    require_fields(command, {"request_id", "action", "order_id"});
    result.operation = ResolveOrder{text(command, "order_id")};
  } else if (action == "live_policy") {
    require_fields(command, {"request_id", "action", "policy", "risk_artifact"});
    result.operation = ChangePolicy{command.at("policy"), text(command, "risk_artifact")};
  } else if (action == "strategy_start") {
    require_fields(command, {"request_id", "action", "venue", "symbol", "strategy",
                             "market_endpoint", "market_service"});
    // Order IDs derived from the run stay within the identity length.
    if (result.id.size() > 64)
      throw std::invalid_argument("strategy run identity is too long");
    auto service = text(command, "market_service");
    validate_id(service);
    result.operation = StartStrategy{{text(command, "venue"), text(command, "symbol")},
                                     protocol::encode_strategy(command.at("strategy")),
                                     text(command, "market_endpoint"),
                                     std::move(service)};
  } else if (action == "strategy_stop") {
    require_fields(command, {"request_id", "action"});
    result.operation = StopStrategy{};
  } else
    throw std::invalid_argument("unsupported live trading operation");
  result.command = command;
  return result;
}
std::string_view AccountRequest::action() const noexcept {
  struct Name {
    std::string_view operator()(const SubmitOrder&) const { return "submit"; }
    std::string_view operator()(const CancelOrder&) const { return "cancel"; }
    std::string_view operator()(const Authorize&) const { return "live_authorize"; }
    std::string_view operator()(const Revoke&) const { return "live_revoke"; }
    std::string_view operator()(const ResolveOrder&) const { return "live_resolve"; }
    std::string_view operator()(const ChangePolicy&) const { return "live_policy"; }
    std::string_view operator()(const StartStrategy&) const { return "strategy_start"; }
    std::string_view operator()(const StopStrategy&) const { return "strategy_stop"; }
  };
  return std::visit(Name{}, operation);
}
std::string_view AccountRequest::order_id() const noexcept {
  if (const auto* submit = as<SubmitOrder>())
    return submit->order.id;
  if (const auto* cancel = as<CancelOrder>())
    return cancel->order_id;
  if (const auto* resolve = as<ResolveOrder>())
    return resolve->order_id;
  return {};
}
Json Authorization::json() const {
  return {{"authorized_at_ms", authorized_at_ms}};
}
JournalRecord parse_record(const Json& record) {
  if (record.is_object() && record.contains("orders_terminal")) {
    require_fields(record, {"orders_terminal"});
    const auto& orders = record.at("orders_terminal");
    if (!orders.is_array() || orders.empty() || orders.size() > OrdersTerminal::batch)
      throw std::invalid_argument("invalid persisted terminal order evidence");
    OrdersTerminal result;
    for (const auto& order : orders)
      result.orders.push_back(terminal_order(order));
    return result;
  }
  if (record.is_object() && record.contains("order_not_sent")) {
    require_fields(record, {"order_not_sent", "error_code"});
    if (!record.at("error_code").is_number_integer() ||
        !not_sent_code(record.at("error_code").get<int>()))
      throw std::invalid_argument("invalid unsent live order result");
    return OrderNotSent{text(record, "order_not_sent"), record.at("error_code").get<int>()};
  }
  if (record.is_object() && record.contains("cancel_result")) {
    require_fields(record, {"cancel_result", "outcome"});
    const auto outcome = text(record, "outcome");
    if (outcome != "dispatch_acknowledged" && outcome != "unknown")
      throw std::invalid_argument("invalid live cancellation result");
    return CancelResult{text(record, "cancel_result"), outcome == "dispatch_acknowledged"};
  }
  if (!record.is_object() || !record.contains("command"))
    throw std::invalid_argument("invalid live trading record");
  const auto& command = record.at("command");
  static_cast<void>(protocol::encode_command(command));
  CommandRecord result{
      AccountRequest::parse(command), text(record, "policy_revision"), {}, {}, {}, {}};
  if (const auto* policy = result.request.as<ChangePolicy>()) {
    require_fields(record, {"command", "policy_revision", "new_policy_revision", "risk_artifact"});
    result.new_policy_revision = text(record, "new_policy_revision");
    validate_id(result.new_policy_revision);
    result.risk_artifact = text(record, "risk_artifact");
    if (policy->risk_artifact != result.risk_artifact)
      throw std::invalid_argument("policy algorithm does not match its recorded artifact");
  } else if (result.request.as<Authorize>())
    require_fields(record, {"command", "policy_revision", "authorization"});
  else if (result.request.as<SubmitOrder>()) {
    require_fields(record, {"command", "policy_revision", "broker_key", "trading_day"});
    result.broker_key = text(record, "broker_key");
    result.trading_day = text(record, "trading_day");
  } else
    require_fields(record, {"command", "policy_revision"});
  return result;
}
JournalEntry command_entry(const AccountRequest& request, std::string_view policy_revision) {
  auto result = command_body(request, policy_revision);
  if (const auto* resolve = request.as<ResolveOrder>())
    result.excluded_order = resolve->order_id;
  return result;
}
JournalEntry submission_entry(const AccountRequest& request, std::string_view policy_revision,
                              std::string_view broker_key, std::string_view trading_day) {
  auto result = command_body(request, policy_revision);
  result.body["broker_key"] = broker_key;
  result.body["trading_day"] = trading_day;
  result.order_id = request.order_id();
  result.trace.broker_key = broker_key;
  return result;
}
JournalEntry policy_entry(const AccountRequest& request, std::string_view policy_revision,
                          std::string_view new_policy_revision, std::string_view risk_artifact) {
  auto result = command_body(request, policy_revision);
  result.body["new_policy_revision"] = new_policy_revision;
  result.body["risk_artifact"] = risk_artifact;
  return result;
}
JournalEntry authorization_entry(const AccountRequest& request, std::string_view policy_revision,
                                 const Authorization& authorization) {
  auto result = command_body(request, policy_revision);
  result.body["authorization"] = authorization.json();
  return result;
}
JournalEntry entry(const OrdersTerminal& record) {
  Json orders = Json::array();
  for (const auto& order : record.orders)
    orders.push_back({{"order_id", order.order_id},
                      {"broker_key", order.broker_key},
                      {"trading_day", order.trading_day},
                      {"status", status_name(order.status)},
                      {"filled", order.filled.str()},
                      {"exchange_order_id", order.exchange_order_id},
                      {"error_code", order.error_code}});
  JournalEntry result;
  result.body = {{"orders_terminal", std::move(orders)}};
  return result;
}
JournalEntry entry(const OrderNotSent& record) {
  JournalEntry result;
  result.body = {{"order_not_sent", record.order_id}, {"error_code", record.error_code}};
  result.excluded_order = record.order_id;
  result.trace.order_id = record.order_id;
  result.trace.outcome = "not_sent";
  return result;
}
JournalEntry entry(const CancelResult& record) {
  JournalEntry result;
  result.trace.request_id = record.request_id;
  result.trace.outcome = record.acknowledged ? "dispatch_acknowledged" : "unknown";
  result.body = {{"cancel_result", record.request_id}, {"outcome", result.trace.outcome}};
  return result;
}
} // namespace asterion::trading
