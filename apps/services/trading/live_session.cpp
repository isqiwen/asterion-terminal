#include "live_session.hpp"
#include <algorithm>
#include <asterion/domain/futures.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/trading.hpp>
#include <chrono>
#include <stdexcept>
namespace asterion::trading {
namespace {
// Record 0 carries this identity. Bump it whenever authorization, allowlist,
// risk or order-recording semantics change; recovery refuses other identities.
const std::string journal_engine = "asterion.live-futures.v1";
constexpr int journal_format = 1;
std::string text(const Json& value, const char* key) {
  auto result = value.at(key).get<std::string>();
  if (result.empty() || result.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid live trading field");
  return result;
}
std::string side_name(Side side) {
  return side == Side::buy ? "buy" : "sell";
}
std::string offset_name(Offset offset) {
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
std::string status_name(BrokerOrderStatus status) {
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
bool working(BrokerOrderStatus status) {
  return status == BrokerOrderStatus::submitted || status == BrokerOrderStatus::accepted ||
         status == BrokerOrderStatus::partially_filled;
}
std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace
LiveSession::LiveSession(std::filesystem::path directory, const std::filesystem::path& ctp_library,
                         const Json& create_manifest)
    : journal_(directory, {"plugins", "ctp-flow"}) {
  journal_.start();
  const auto records = journal_.read();
  if (!create_manifest.is_null()) {
    if (!records.empty())
      throw std::invalid_argument("directory already holds a session; recover it instead");
    manifest_ = protocol::decode_live_input(protocol::encode_live_input(create_manifest));
    risk_module_ = risk_providers::Module::selected();
  } else {
    if (records.empty())
      throw std::invalid_argument("directory holds no recoverable live session");
    const auto& header = records.front();
    if (!header.is_object() || !header.contains("format") || header.at("format") != journal_format)
      throw std::invalid_argument("unsupported live trading record format; this build reads "
                                  "format 1 only and leaves the directory unchanged");
    require_fields(header, {"format", "engine", "risk_artifact", "manifest"});
    if (header.at("engine") != journal_engine)
      throw std::invalid_argument("live trading record was written by engine " +
                                  header.at("engine").dump() + " but this build implements " +
                                  journal_engine + "; recovery refused");
    manifest_ = protocol::decode_live_input(protocol::encode_live_input(header.at("manifest")));
    risk_module_ = risk_providers::Module::pinned(directory / "plugins",
                                                  header.at("risk_artifact").get<std::string>());
  }
  const auto input = protocol::encode_live_input(manifest_);
  for (const auto& c : input.contracts()) {
    const auto terms = protocol::instrument(c);
    FuturesContract{terms, c.product(), c.delivery_month()}.validate();
    contracts_.push_back(terms);
  }
  auto risk = risk_module_->create(decode_order_limits(manifest_.at("risk")));
  risk->start();
  risk_ = std::move(risk);
  trader_ = std::make_unique<ctp::Trader>(ctp_library, directory / "ctp-flow");
  trader_->start();
  if (!create_manifest.is_null()) {
    if (!std::filesystem::create_directory(directory / "plugins"))
      throw std::invalid_argument("risk plugin snapshot already exists or directory is invalid");
    risk_module_->capture(directory / "plugins");
    journal_.append({{"format", journal_format},
                     {"engine", journal_engine},
                     {"risk_artifact", risk_module_->artifact()},
                     {"manifest", manifest_}});
    return;
  }
  for (std::size_t i = 1; i < records.size(); ++i) {
    const auto& record = records[i];
    if (!record.is_object() || !record.contains("command"))
      throw std::invalid_argument("invalid live trading record");
    const auto& command = record.at("command");
    const auto id = text(command, "request_id");
    if (!commands_.emplace(id, command).second)
      throw std::invalid_argument("live trading record repeats a request");
    const auto action = text(command, "action");
    if (action == "live_authorize")
      require_fields(record, {"command", "authorization"});
    else if (action != "submit")
      require_fields(record, {"command"});
    else {
      require_fields(record, {"command", "broker_key", "trading_day"});
      intents_[text(command, "order_id")] = {text(record, "broker_key"),
                                             text(record, "trading_day"),
                                             {text(command, "venue"), text(command, "symbol")},
                                             offset_of(text(command, "offset")),
                                             Decimal::parse(text(command, "quantity"))};
    }
  }
}
LiveSession::~LiveSession() {
  if (trader_)
    trader_->disconnect();
}
void LiveSession::append(const Json& record) {
  try {
    journal_.append(record);
  } catch (...) {
    failed_ = true;
    throw;
  }
}
void LiveSession::connect(std::string password, std::string auth_code) {
  if (failed_)
    throw std::invalid_argument("live trading record needs recovery; reopen the session");
  if (password.empty() || auth_code.empty())
    throw std::invalid_argument("enter the CTP password and authentication code");
  const auto& broker = manifest_.at("broker");
  std::map<std::string, std::string> known;
  for (const auto& [order_id, intent] : intents_)
    known[intent.broker_key] = order_id;
  authorization_ = nullptr;
  trader_->connect({text(broker, "front"), text(broker, "broker_id"), text(broker, "user_id"),
                    std::move(password), text(broker, "app_id"), std::move(auth_code)},
                   std::move(known));
}
void LiveSession::disconnect() {
  authorization_ = nullptr;
  trader_->disconnect();
}
const Instrument& LiveSession::allowed(const InstrumentId& id) const {
  for (const auto& terms : contracts_)
    if (terms.id == id)
      return terms;
  throw std::invalid_argument("contract is not allowed in this live session");
}
// Orders this session recorded for the broker's current trading day that the
// synchronized broker does not report.
std::vector<std::pair<std::string, const LiveSession::Intent*>>
LiveSession::unconfirmed(const BrokerSnapshot& state) const {
  std::vector<std::pair<std::string, const Intent*>> result;
  if (state.phase != "ready")
    return result;
  for (const auto& [order_id, intent] : intents_) {
    if (intent.trading_day != state.trading_day)
      continue;
    const bool reported = std::ranges::any_of(state.orders, [&](const BrokerOrder& order) {
      return order.broker_key == intent.broker_key;
    });
    if (!reported)
      result.emplace_back(order_id, &intent);
  }
  return result;
}
void LiveSession::execute(const Json& command) {
  if (failed_)
    throw std::invalid_argument("live trading record needs recovery; reopen the session");
  const auto id = text(command, "request_id");
  validate_id(id);
  const auto action = text(command, "action");
  if (const auto found = commands_.find(id); found != commands_.end()) {
    // A retried order or cancel is acknowledged, never sent again. An
    // authorization is never acknowledged from history: it may have ended.
    if (found->second != command || action == "live_authorize" || action == "live_revoke")
      throw Error(ErrorCode::conflict, "request ID was already used");
    return;
  }
  if (action == "live_authorize") {
    require_fields(command, {"request_id", "action", "user_id"});
    if (command.at("user_id") != manifest_.at("broker").at("user_id"))
      throw std::invalid_argument("authorization names a different account");
    const auto state = trader_->snapshot();
    if (state.phase != "ready")
      throw Error(ErrorCode::unavailable, "connect and synchronize the account before authorizing");
    Json authorization{{"trading_day", state.trading_day}, {"authorized_at_ms", now_ms()}};
    append({{"command", command}, {"authorization", authorization}});
    commands_.emplace(id, command);
    authorization_ = std::move(authorization);
  } else if (action == "live_revoke") {
    require_fields(command, {"request_id", "action"});
    if (authorization_.is_null())
      throw std::invalid_argument("live trading is not authorized");
    append({{"command", command}});
    commands_.emplace(id, command);
    authorization_ = nullptr;
  } else if (action == "submit") {
    submit(command);
  } else if (action == "cancel") {
    require_fields(command, {"request_id", "action", "order_id"});
    const auto order_id = text(command, "order_id");
    if (!intents_.contains(order_id))
      throw std::invalid_argument("order was not placed by this session");
    trader_->cancel(order_id);
    append({{"command", command}});
    commands_.emplace(id, command);
  } else
    throw std::invalid_argument("unsupported live trading operation");
}
void LiveSession::submit(const Json& command) {
  require_fields(command, {"request_id", "action", "order_id", "venue", "symbol", "side", "offset",
                           "quantity", "price"});
  const auto order_id = text(command, "order_id");
  validate_id(order_id);
  if (intents_.contains(order_id))
    throw Error(ErrorCode::conflict, "order ID was already used");
  const auto state = trader_->snapshot();
  if (authorization_.is_null())
    throw std::invalid_argument("authorize live trading for this account first");
  if (state.phase != "ready")
    throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
  if (authorization_.at("trading_day") != state.trading_day)
    throw std::invalid_argument("the trading day changed; authorize live trading again");
  const InstrumentId id{text(command, "venue"), text(command, "symbol")};
  const auto& terms = allowed(id);
  const auto side = text(command, "side");
  if (side != "buy" && side != "sell")
    throw std::invalid_argument("invalid order side");
  const auto offset = offset_of(text(command, "offset"));
  const LimitOrder order{order_id, id, side == "buy" ? Side::buy : Side::sell,
                         Decimal::parse(text(command, "quantity")),
                         Decimal::parse(text(command, "price"))};
  if (order.quantity <= Decimal{} || !order.quantity.multiple_of(terms.quantity_increment))
    throw std::invalid_argument("order quantity must be a positive multiple of the lot size");
  if (order.limit_price <= Decimal{} || !order.limit_price.multiple_of(terms.price_increment))
    throw std::invalid_argument("limit price must be a positive multiple of the price tick");
  // Exposure from broker reports, plus recorded orders the broker has not
  // confirmed: they may still reach the exchange.
  Decimal gross, pending;
  std::size_t working_orders = 0;
  for (const auto& position : state.positions)
    if (position.instrument == id)
      gross = gross + position.today + position.yesterday;
  for (const auto& o : state.orders) {
    if (!working(o.status))
      continue;
    ++working_orders;
    if (o.instrument == id && o.offset == Offset::open)
      pending = pending + (o.quantity - o.filled);
  }
  for (const auto& [unconfirmed_id, intent] : unconfirmed(state)) {
    static_cast<void>(unconfirmed_id);
    ++working_orders;
    if (intent->instrument == id && intent->offset == Offset::open)
      pending = pending + intent->quantity;
  }
  const auto decision = risk_->evaluate({terms, order, offset, gross, pending, working_orders});
  if (!decision.allowed())
    throw std::invalid_argument("pre-trade risk rejected: " +
                                std::string(risk_reason_name(decision.reason)));
  // The order is recorded durably before the trader sends anything.
  trader_->submit(order, offset, [&](const BrokerOrder& pending_order) {
    append({{"command", command},
            {"broker_key", pending_order.broker_key},
            {"trading_day", state.trading_day}});
    commands_.emplace(text(command, "request_id"), command);
    intents_[order_id] = {pending_order.broker_key, state.trading_day, id, offset, order.quantity};
  });
}
Json LiveSession::snapshot() const {
  const auto state = trader_->snapshot();
  Json result{{"broker", manifest_.at("broker")},
              {"risk", manifest_.at("risk")},
              {"contracts", manifest_.at("contracts")},
              {"phase", state.phase},
              {"error_code", state.error_code},
              {"trading_day", state.trading_day},
              {"synchronized_ms", state.synchronized_ms},
              {"funds", nullptr},
              {"positions", Json::array()},
              {"orders", Json::array()},
              {"trades", Json::array()},
              {"authorization", authorization_},
              {"unconfirmed", Json::array()},
              {"storage_state", failed_ ? "recovery_required" : "ready"}};
  if (state.funds)
    result["funds"] = {{"balance", state.funds->balance.str()},
                       {"available", state.funds->available.str()},
                       {"margin", state.funds->margin.str()},
                       {"commission", state.funds->commission.str()},
                       {"close_profit", state.funds->close_profit.str()},
                       {"position_profit", state.funds->position_profit.str()}};
  for (const auto& p : state.positions)
    result["positions"].push_back({{"venue", p.instrument.venue},
                                   {"symbol", p.instrument.symbol},
                                   {"side", side_name(p.side)},
                                   {"today", p.today.str()},
                                   {"yesterday", p.yesterday.str()}});
  for (const auto& o : state.orders)
    result["orders"].push_back({{"id", o.order_id},
                                {"broker_key", o.broker_key},
                                {"exchange_order_id", o.exchange_order_id},
                                {"venue", o.instrument.venue},
                                {"symbol", o.instrument.symbol},
                                {"side", side_name(o.side)},
                                {"offset", offset_name(o.offset)},
                                {"quantity", o.quantity.str()},
                                {"filled", o.filled.str()},
                                {"limit_price", o.limit_price.str()},
                                {"status", status_name(o.status)},
                                {"error_code", o.error_code}});
  for (const auto& t : state.trades)
    result["trades"].push_back({{"id", t.trade_id},
                                {"order_id", t.order_id},
                                {"venue", t.instrument.venue},
                                {"symbol", t.instrument.symbol},
                                {"side", side_name(t.side)},
                                {"offset", offset_name(t.offset)},
                                {"quantity", t.quantity.str()},
                                {"price", t.price.str()},
                                {"trading_day", t.trading_day},
                                {"trade_time", t.trade_time}});
  for (const auto& [order_id, intent] : unconfirmed(state))
    result["unconfirmed"].push_back({{"id", order_id},
                                     {"broker_key", intent->broker_key},
                                     {"trading_day", intent->trading_day}});
  return result;
}
} // namespace asterion::trading
