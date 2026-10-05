#include <asterion/kernel/logger.hpp>
#include "live_account_state.hpp"
#include <algorithm>
#include <asterion/domain/futures.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/trading.hpp>
#include <chrono>
#include <set>
#include <fstream>
#include <stdexcept>
namespace asterion::trading {
namespace {
// Record 0 carries this identity. Bump it whenever authorization, allowlist,
// risk or order-recording semantics change; recovery refuses other identities.
// v25: execution ownership follows the stable account record ID.
const std::string journal_engine = "asterion.live-futures.v25";
constexpr int journal_format = 1;
constexpr auto quote_validity = std::chrono::seconds(10);
constexpr std::size_t terminal_batch = 32;
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
Json manifest_from_header(const Json& header) {
  if (!header.is_object() || !header.contains("format") || header.at("format") != journal_format)
    throw std::invalid_argument("unsupported live trading record format; this build reads "
                                "format 1 only and leaves the directory unchanged");
  if (header.at("engine") != journal_engine)
    throw std::invalid_argument("live trading record was written by engine " +
                                header.at("engine").dump() + " but this build implements " +
                                journal_engine + "; recovery refused");
  require_fields(
      header, {"format", "engine", "risk_artifact", "manifest", "account_id", "policy_revision"});
  validate_id(text(header, "account_id"));
  validate_id(text(header, "policy_revision"));
  auto manifest = protocol::decode_live_input(protocol::encode_live_input(header.at("manifest")));
  if (manifest.at("account_id") != header.at("account_id"))
    throw std::invalid_argument("live account identity differs from record header");
  return manifest;
}
void validate_header(const Json& header) {
  static_cast<void>(manifest_from_header(header));
}
} // namespace
LiveAccountState::LiveAccountState(std::filesystem::path directory,
                                   const std::filesystem::path& ctp_library,
                                   const std::filesystem::path& ownership_directory,
                                   const Json& create_manifest, AccountJournal::Post post,
                                   std::function<void()> broker_ready, BrokerSendGate& send_gate,
                                   Progress& persistence)
    : directory_(directory), send_gate_(send_gate) {
  Json manifest;
  if (!create_manifest.is_null()) {
    manifest = protocol::decode_live_input(protocol::encode_live_input(create_manifest));
    if (std::filesystem::exists(directory / "journal.sqlite"))
      throw std::invalid_argument("directory already holds a session; recover it instead");
  } else {
    header_ = read_journal_header(directory, {"plugins", "ctp-flow", "archives"});
    validate_header(header_);
    manifest = manifest_from_header(header_);
  }
  const auto& broker = manifest.at("broker");
  broker_id_ = broker.at("broker_id");
  investor_id_ = broker.at("user_id");
  const auto account_id = text(manifest, "account_id");
  trade_front_ = broker.at("front");
  app_id_ = broker.at("app_id");
  create_directories_durably(ownership_directory);
  try {
    account_owner_ = std::make_unique<FileLock>(ownership_directory, sha256_bytes(account_id));
  } catch (const std::runtime_error&) {
    throw Error(ErrorCode::conflict,
                "account execution owner is already active or ownership storage is unavailable");
  }
  const auto binding_file = ownership_directory / (sha256_bytes(account_id) + ".json");
  Json binding{{"version", 2},
               {"directory", std::filesystem::canonical(directory).string()},
               {"initialized", false}};
  if (std::filesystem::exists(binding_file)) {
    if (std::filesystem::is_symlink(binding_file) ||
        !std::filesystem::is_regular_file(binding_file) ||
        std::filesystem::file_size(binding_file) > 16384)
      throw std::invalid_argument("invalid account ownership binding");
    std::ifstream source(binding_file);
    const auto stored = parse_json(std::string(std::istreambuf_iterator<char>(source), {}), 16384);
    require_fields(stored, {"version", "directory", "initialized"});
    if (stored.at("version") != 2 || !stored.at("initialized").is_boolean())
      throw std::invalid_argument("invalid account ownership binding");
    if (stored.at("directory") != binding.at("directory"))
      throw Error(ErrorCode::conflict, "this account is bound to another trading directory");
    if (stored.at("initialized") == true && !create_manifest.is_null())
      throw Error(ErrorCode::conflict,
                  "this account is initialized; recover its original trading record");
    binding = stored;
  } else {
    // Claim before any writable journal or SDK work. A failed initial creation
    // may be retried only at this location for this account record.
    replace_file_durably(binding_file, binding.dump());
  }
  const auto finish_initialization = [&] {
    if (binding.at("initialized") == false) {
      binding["initialized"] = true;
      replace_file_durably(binding_file, binding.dump());
    }
  };
  // Acquire the account before opening its journal for writing or creating an SDK instance.
  journal_ = std::make_unique<AccountJournal>(
      directory, [](const Json& header) { validate_header(header); }, std::move(post), persistence);
  capacity_ = journal_->initial_capacity();
  auto definition = manifest.at("policy");
  auto revision =
      create_manifest.is_null() ? text(header_, "policy_revision") : unique_process_id();
  auto artifact = create_manifest.is_null() ? text(header_, "risk_artifact") : std::string{};
  std::set<std::string> revisions{revision};
  std::set<std::string> pending_cancels;
  journal_->replay([&](std::uint64_t sequence, const Json& record) {
    if (sequence == 0) {
      if (record != header_)
        throw Error(ErrorCode::conflict,
                    "account record changed while acquiring execution ownership");
      return;
    }
    if (record.is_object() && record.contains("orders_terminal")) {
      require_fields(record, {"orders_terminal"});
      const auto& orders = record.at("orders_terminal");
      if (!orders.is_array() || orders.empty() || orders.size() > terminal_batch)
        throw std::invalid_argument("invalid persisted terminal order evidence");
      for (const auto& order : orders) {
        const auto found = intents_.find(text(order, "order_id"));
        if (found == intents_.end())
          throw std::invalid_argument("invalid persisted terminal order evidence");
        validate_terminal(order, found->second);
        intents_.erase(found);
      }
      return;
    }
    if (record.is_object() && record.contains("order_not_sent")) {
      require_fields(record, {"order_not_sent", "error_code"});
      const auto found = intents_.find(text(record, "order_not_sent"));
      if (found == intents_.end() || !record.at("error_code").is_number_integer() ||
          (record.at("error_code") != -1003 && record.at("error_code") != -1004 &&
           record.at("error_code") != -1005 && record.at("error_code") != -1006 &&
           record.at("error_code") != -1007))
        throw std::invalid_argument("invalid unsent live order result");
      intents_.erase(found);
      return;
    }
    if (record.is_object() && record.contains("cancel_result")) {
      require_fields(record, {"cancel_result", "outcome"});
      const auto id = text(record, "cancel_result");
      const auto outcome = text(record, "outcome");
      if (!pending_cancels.erase(id) ||
          (outcome != "dispatch_acknowledged" && outcome != "unknown"))
        throw std::invalid_argument("invalid live cancellation result");
      return;
    }
    if (!record.is_object() || !record.contains("command"))
      throw std::invalid_argument("invalid live trading record");
    const auto& command = record.at("command");
    static_cast<void>(protocol::encode_command(command));
    const auto id = text(command, "request_id");
    if (text(record, "policy_revision") != revision)
      throw std::invalid_argument("trading command policy does not match the active revision");
    const auto action = text(command, "action");
    if (action == "cancel")
      pending_cancels.insert(id);
    if (action == "live_policy") {
      require_fields(record,
                     {"command", "policy_revision", "new_policy_revision", "risk_artifact"});
      require_fields(command, {"request_id", "action", "policy", "risk_artifact"});
      revision = text(record, "new_policy_revision");
      validate_id(revision);
      if (!revisions.insert(revision).second)
        throw std::invalid_argument("account policy revision is repeated");
      definition = protocol::decode_live_policy(protocol::encode_live_policy(command.at("policy")));
      artifact = text(record, "risk_artifact");
      if (command.at("risk_artifact") != artifact)
        throw std::invalid_argument("policy algorithm does not match its recorded artifact");
    } else if (action == "live_authorize")
      require_fields(record, {"command", "policy_revision", "authorization"});
    else if (action == "live_resolve") {
      require_fields(record, {"command", "policy_revision"});
      const auto found = intents_.find(text(command, "order_id"));
      if (found == intents_.end())
        throw std::invalid_argument("live trading record resolves an unknown order");
      intents_.erase(found);
    } else if (action != "submit")
      require_fields(record, {"command", "policy_revision"});
    else {
      require_fields(record, {"command", "policy_revision", "broker_key", "trading_day"});
      intents_[text(command, "order_id")] = {text(record, "broker_key"),
                                             text(record, "trading_day"),
                                             {text(command, "venue"), text(command, "symbol")},
                                             offset_of(text(command, "offset")),
                                             Decimal::parse(text(command, "quantity"))};
    }
  });
  auto algorithm = create_manifest.is_null()
                       ? risk_providers::Module::pinned(directory / "plugins" / artifact, artifact)
                       : risk_providers::Module::selected();
  policy_ = std::make_unique<AccountPolicy>(definition, revision, std::move(algorithm));
  trader_ = std::make_unique<ctp::Trader>(ctp_library, directory / "ctp-flow", send_gate_,
                                          std::move(broker_ready));
  trader_->start();
  if (!create_manifest.is_null()) {
    policy_->capture(directory / "plugins");
    header_ = {{"format", journal_format},
               {"engine", journal_engine},
               {"risk_artifact", policy_->algorithm.artifact()},
               {"manifest", manifest},
               {"policy_revision", policy_->revision},
               {"account_id", account_id}};
    capacity_ = journal_->initialize(header_);
  }
  finish_initialization();
}
LiveAccountState::~LiveAccountState() {
  if (trader_)
    trader_->disconnect();
}
AccountCommand LiveAccountState::append(Json record) {
  try {
    if (record.contains("command"))
      record["policy_revision"] = policy_->revision;
    capacity_ = (co_await journal_->append(record)).capacity;
  } catch (...) {
    failed_ = true;
    throw;
  }
  try {
    const auto identity = [](const Json& value, const char* name) -> std::string_view {
      return value.contains(name) ? value.at(name).get_ref<const std::string&>()
                                  : std::string_view{};
    };
    const auto& command = record.contains("command") ? record.at("command") : record;
    log_identity_event(
        "trading", "journal.committed",
        {{"account_id", identity(header_, "account_id")},
         {"request_id", record.contains("cancel_result") ? identity(record, "cancel_result")
                                                         : identity(command, "request_id")},
         {"action", identity(command, "action")},
         {"order_id", record.contains("order_not_sent") ? identity(record, "order_not_sent")
                                                        : identity(command, "order_id")},
         {"broker_key", identity(record, "broker_key")},
         {"outcome",
          record.contains("order_not_sent") ? "not_sent" : identity(record, "outcome")}});
  } catch (...) {
    log_process_failure("trading", "trace.failed", ErrorCode::internal_error, 1);
  }
}
void LiveAccountState::log_broker_observations(const BrokerSnapshot& state) const noexcept {
  try {
    if (!process_logger("trading") || (observed_generation_ == state.connection_generation &&
                                       observed_sequence_ == state.sequence))
      return;
    if (observed_generation_ != state.connection_generation) {
      observed_orders_.clear();
      observed_trades_ = 0;
      observed_generation_ = state.connection_generation;
    }
    const auto& record = header_.at("account_id").get_ref<const std::string&>();
    for (const auto& order : state.orders) {
      ObservedOrder observation{order.status, order.filled, order.exchange_order_id,
                                order.order_id};
      auto [found, inserted] = observed_orders_.try_emplace(order.broker_key, observation);
      if (!inserted && found->second == observation)
        continue;
      found->second = std::move(observation);
      const auto status = status_name(order.status);
      log_identity_event("trading", "broker.order_observed",
                         {{"account_id", record},
                          {"trading_day", state.trading_day},
                          {"order_id", order.order_id},
                          {"broker_key", order.broker_key},
                          {"exchange_order_id", order.exchange_order_id},
                          {"status", status}});
    }
    if (observed_trades_ > state.trades.size())
      observed_trades_ = 0;
    for (; observed_trades_ < state.trades.size(); ++observed_trades_) {
      const auto& trade = state.trades[observed_trades_];
      log_identity_event("trading", "broker.trade_observed",
                         {{"account_id", record},
                          {"order_id", trade.order_id},
                          {"exchange_order_id", trade.exchange_order_id},
                          {"trade_id", trade.trade_id},
                          {"trading_day", trade.trading_day}});
    }
    observed_sequence_ = state.sequence;
  } catch (...) {
    log_process_failure("trading", "trace.failed", ErrorCode::internal_error, 1);
  }
}
void LiveAccountState::connect(std::string password, std::string auth_code) {
  if (failed_)
    throw std::invalid_argument("live trading record needs recovery; reopen the session");
  if (password.empty() || auth_code.empty())
    throw std::invalid_argument("enter the CTP password and authentication code");
  authorization_ = nullptr;
  send_gate_.invalidate();
  trader_->connect(
      {trade_front_, broker_id_, investor_id_, std::move(password), app_id_, std::move(auth_code)});
}
void LiveAccountState::query_costs() {
  std::vector<std::pair<InstrumentId, std::string>> contracts;
  for (const auto& c : policy_->contracts)
    contracts.emplace_back(c.instrument.id, c.product);
  trader_->query_costs(contracts);
}
void LiveAccountState::disconnect() {
  authorization_ = nullptr;
  send_gate_.invalidate();
  trader_->disconnect();
}
const Instrument& LiveAccountState::allowed(const InstrumentId& id) const {
  if (const auto* contract = policy_->find(id))
    return contract->instrument;
  throw std::invalid_argument("contract is not allowed in this live session");
}
// Unretired durable intents which the synchronized broker does not report.
// A new trading day cannot prove that a previous day's unknown command was absent.
std::vector<std::pair<std::string, const LiveAccountState::Intent*>>
LiveAccountState::unconfirmed(const BrokerSnapshot& state) const {
  std::vector<std::pair<std::string, const Intent*>> result;
  if (state.phase != "ready" || !identities_ready(state))
    return result;
  for (const auto& [order_id, intent] : intents_) {
    const bool reported = std::ranges::any_of(state.orders, [&](const BrokerOrder& order) {
      return intent.trading_day == state.trading_day && order.broker_key == intent.broker_key &&
             order.order_id == order_id;
    });
    if (!reported)
      result.emplace_back(order_id, &intent);
  }
  return result;
}
void LiveAccountState::check_account(std::string_view account_id) const {
  if (account_id.empty() || account_id != header_.at("account_id").get_ref<const std::string&>())
    throw Error(
        ErrorCode::conflict,
        "trading account identity does not match; review the current account before acting");
}
AccountCommand LiveAccountState::revise_policy(const Json& command) {
  require_fields(command, {"request_id", "action", "policy", "risk_artifact"});
  auto prepared = co_await journal_->prepare_policy(
      directory_ / "plugins", command.at("policy"), unique_process_id(),
      text(command, "risk_artifact"), policy_->algorithm.artifact());
  auto next = std::move(prepared.policy);
  std::exception_ptr failure;
  try {
    const auto state = trader_->snapshot();
    if (state.phase != "ready" || !identities_ready(state) || !state.positions_reconciled ||
        !unconfirmed(state).empty())
      throw Error(ErrorCode::conflict,
                  "policy change requires reconciled positions and no unconfirmed orders");
    for (const auto& position : state.positions)
      if (position.today + position.yesterday > Decimal{})
        next->preserve_exposure(*policy_, position.instrument);
    for (const auto& order : state.orders)
      if (working(order.status))
        next->preserve_exposure(*policy_, order.instrument);
    Json record{{"command", command},
                {"new_policy_revision", next->revision},
                {"risk_artifact", next->algorithm.artifact()}};
    trader_->disconnect_checked(state.connection_generation, state.exposure_revision);
    authorization_ = nullptr;
    send_gate_.invalidate();
    co_await append(std::move(record));
    policy_.swap(next);
  } catch (...) {
    failure = std::current_exception();
  }
  // Both a rejected candidate and a replaced policy release their plugin on the writer.
  co_await journal_->retire_policy(std::move(next));
  if (failure)
    std::rethrow_exception(failure);
}
void LiveAccountState::admit_revoke(std::string_view account_id, std::string_view revision,
                                    const Json& command) {
  check_account(account_id);
  require_fields(command, {"request_id", "action"});
  const auto id = text(command, "request_id");
  validate_id(id);
  if (revision != policy_->revision)
    throw Error(ErrorCode::conflict,
                "account policy changed; review the current revision before acting");
  // The entry point already closed permission. Only the owner changes authorization.
  authorization_ = nullptr;
}
AccountCommand LiveAccountState::execute(std::string account_id, std::string policy_revision,
                                         Json command, std::uint64_t admitted_control) {
  check_account(account_id);
  if (failed_)
    throw std::invalid_argument("live trading record needs recovery; reopen the session");
  const auto id = text(command, "request_id");
  validate_id(id);
  const auto action = text(command, "action");
  Json recorded;
  try {
    recorded = (co_await journal_->find_command(id)).record;
  } catch (...) {
    failed_ = true;
    throw;
  }
  if (!recorded.is_null()) {
    // A retried order or cancel is acknowledged, never sent again. An
    // authorization is never acknowledged from history: it may have ended.
    if (recorded.at("command") != command || recorded.at("policy_revision") != policy_revision ||
        action == "live_authorize" || action == "live_revoke")
      throw Error(ErrorCode::conflict, "request ID was already used");
    co_return;
  }
  if (policy_revision != policy_->revision)
    throw Error(ErrorCode::conflict,
                "account policy changed; review the current revision before acting");
  if ((action == "live_authorize" || action == "submit") &&
      admitted_control != send_gate_.revision())
    throw Error(ErrorCode::conflict,
                "account control changed after command admission; submit a new request");
  if (action == "live_policy") {
    co_await revise_policy(command);
  } else if (action == "live_authorize") {
    require_fields(command, {"request_id", "action", "user_id"});
    if (command.at("user_id") != investor_id_)
      throw std::invalid_argument("authorization names a different account");
    const auto state = trader_->snapshot();
    if (state.phase != "ready" || !identities_ready(state))
      throw Error(ErrorCode::unavailable, "connect and synchronize the account before authorizing");
    const auto control = admitted_control;
    Json authorization{{"trading_day", state.trading_day}, {"authorized_at_ms", now_ms()}};
    Json record{{"command", command}, {"authorization", authorization}};
    co_await append(std::move(record));
    if (control != send_gate_.revision() ||
        state.connection_generation != trader_->snapshot().connection_generation)
      throw Error(ErrorCode::conflict, "account state changed before durable grant completed");
    authorization_ = std::move(authorization);
    authorization_generation_ = state.connection_generation;
  } else if (action == "live_revoke") {
    require_fields(command, {"request_id", "action"});
    Json record{{"command", command}};
    co_await append(std::move(record));
  } else if (action == "live_resolve") {
    // Only an order listed as unconfirmed right now: the broker is
    // synchronized and does not report it.
    require_fields(command, {"request_id", "action", "order_id"});
    const auto order_id = text(command, "order_id");
    const auto listed = unconfirmed(trader_->snapshot());
    if (std::ranges::none_of(listed, [&](const auto& item) { return item.first == order_id; }))
      throw std::invalid_argument("only an unconfirmed order of a synchronized account can be "
                                  "resolved");
    Json record{{"command", command}};
    co_await append(std::move(record));
    intents_.erase(order_id);
  } else if (action == "submit") {
    co_await submit(command, admitted_control);
  } else if (action == "cancel") {
    require_fields(command, {"request_id", "action", "order_id"});
    const auto order_id = text(command, "order_id");
    if (!identities_ready(trader_->snapshot()))
      throw Error(ErrorCode::unavailable, "account identity reconciliation is not complete");
    if (!intents_.contains(order_id))
      throw Error(ErrorCode::conflict, "order is not active in this account");
    Json record{{"command", command}};
    co_await append(std::move(record));
    std::exception_ptr failure;
    try {
      const auto result = co_await wait_sdk(trader_->cancel(order_id));
      if (!result.invoked || result.code)
        throw Error(ErrorCode::operation_failed,
                    "CTP cancel request failed with code " + std::to_string(result.code));
    } catch (...) {
      failure = std::current_exception();
    }
    // SDK acceptance is not exchange cancellation. Broker reports own status.
    Json outcome{{"cancel_result", id}, {"outcome", failure ? "unknown" : "dispatch_acknowledged"}};
    co_await append(std::move(outcome));
    if (failure)
      std::rethrow_exception(failure);
  } else
    throw std::invalid_argument("unsupported live trading operation");
}
// The broker's current market bounds the limit price: within the exchange's
// limits and within the session's deviation from the latest price (the
// pre-settlement price before the first trade). No market, no order.
void LiveAccountState::check_price(const LimitOrder& order, const std::optional<BrokerQuote>& quote,
                                   const BrokerSnapshot& state) const {
  if (quote && (quote->instrument != order.instrument || quote->trading_day != state.trading_day ||
                quote->connection_generation != state.connection_generation ||
                std::chrono::steady_clock::now() >= quote->completed_at + quote_validity))
    throw Error(
        ErrorCode::unavailable,
        "broker quote is expired or belongs to another trading session; the order was not sent");
  const auto reference = !quote ? std::nullopt : quote->last ? quote->last : quote->pre_settlement;
  if (!reference || *reference <= Decimal{})
    throw std::invalid_argument(
        "no current market price for this contract; the order was not sent");
  if ((quote->upper_limit && order.limit_price > *quote->upper_limit) ||
      (quote->lower_limit && order.limit_price < *quote->lower_limit))
    throw std::invalid_argument("limit price is outside the exchange price limits");
  const auto distance = order.limit_price > *reference ? order.limit_price - *reference
                                                       : *reference - order.limit_price;
  // Rounded: an exact product could exceed eight decimal places.
  const auto bound = multiply(*reference, policy_->max_price_deviation, Rounding::half_up);
  if (distance > bound)
    throw std::invalid_argument(
        "limit price deviates from the latest price beyond the session limit");
}
AccountCommand LiveAccountState::submit(const Json& command, std::uint64_t control) {
  require_fields(command, {"request_id", "action", "order_id", "venue", "symbol", "side", "offset",
                           "quantity", "price"});
  const auto order_id = text(command, "order_id");
  validate_id(order_id);
  Json previous;
  try {
    previous = (co_await journal_->find_order(order_id)).record;
  } catch (...) {
    failed_ = true;
    throw;
  }
  if (!previous.is_null())
    throw Error(ErrorCode::conflict, "order ID was already used");
  const auto before = trader_->snapshot();
  if (!authorized(before))
    throw std::invalid_argument("authorize live trading for this account first");
  if (before.phase != "ready")
    throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
  if (authorization_.at("trading_day") != before.trading_day)
    throw std::invalid_argument("the trading day changed; authorize live trading again");
  const InstrumentId id{text(command, "venue"), text(command, "symbol")};
  const auto& terms = allowed(id);
  const auto side = text(command, "side");
  if (side != "buy" && side != "sell")
    throw std::invalid_argument("invalid order side");
  const auto offset = offset_of(text(command, "offset"));
  if (offset == Offset::open && !before.positions_reconciled)
    throw Error(ErrorCode::unavailable,
                "broker fills are awaiting position reconciliation; opening orders are paused");
  const LimitOrder order{order_id, id, side == "buy" ? Side::buy : Side::sell,
                         Decimal::parse(text(command, "quantity")),
                         Decimal::parse(text(command, "price"))};
  if (order.quantity <= Decimal{} || !order.quantity.multiple_of(terms.quantity_increment))
    throw std::invalid_argument("order quantity must be a positive multiple of the lot size");
  if (order.limit_price <= Decimal{} || !order.limit_price.multiple_of(terms.price_increment))
    throw std::invalid_argument("limit price must be a positive multiple of the price tick");
  const auto quote = co_await wait_sdk(trader_->quote(id));
  // The quote waits for the CTP query limit; risk reads the account after it.
  const auto state = trader_->snapshot();
  if (!authorized(state))
    throw Error(ErrorCode::unavailable, "CTP trading session changed; the order was not sent");
  if (offset == Offset::open && !state.positions_reconciled)
    throw Error(ErrorCode::unavailable,
                "broker fills are awaiting position reconciliation; opening orders are paused");
  check_price(order, quote, state);
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
  const auto decision =
      policy_->risk->evaluate({terms, order, offset, gross, pending, working_orders});
  if (!decision.allowed())
    throw std::invalid_argument("pre-trade risk rejected: " +
                                std::string(risk_reason_name(decision.reason)));
  // The order is recorded durably before the trader sends anything.
  const auto deadline = quote->completed_at + quote_validity;
  auto prepared =
      trader_->prepare(order, offset, authorization_generation_, state.exposure_revision, deadline);
  const auto& prepared_order = prepared->order();
  check_price(order, quote, trader_->snapshot());
  Json record{{"command", command},
              {"broker_key", prepared_order.broker_key},
              {"trading_day", state.trading_day}};
  co_await append(std::move(record));
  const auto sequence = capacity_.total_records - 1;
  intents_[order_id] = {prepared_order.broker_key, state.trading_day, id, offset, order.quantity};
  if (control != send_gate_.revision() || !authorized(trader_->snapshot())) {
    Json refused{{"order_not_sent", order_id}, {"error_code", -1007}};
    co_await append(std::move(refused));
    intents_.erase(order_id);
    throw Error(ErrorCode::conflict,
                "order execution basis changed during durable submission; the order was not sent");
  }
  std::optional<BrokerSendPermit> permit;
  try {
    permit.emplace(send_gate_.issue(control, order_id));
  } catch (const Error&) {
    // An ingress producer can invalidate between the last check and this CAS.
  }
  if (!permit) {
    Json refused{{"order_not_sent", order_id}, {"error_code", -1007}};
    co_await append(std::move(refused));
    intents_.erase(order_id);
    throw Error(ErrorCode::conflict,
                "order permission changed before dispatch; the order was not sent");
  }
  const auto result =
      co_await wait_sdk(trader_->dispatch(std::move(prepared), std::move(*permit), sequence));
  if (!result.invoked) {
    // Only positive local evidence can release the recorded intent. If this
    // append fails, recovery retains it as an unknown order.
    Json refused{{"order_not_sent", order_id}, {"error_code", result.code}};
    co_await append(std::move(refused));
    intents_.erase(order_id);
    throw Error(ErrorCode::conflict,
                "order dispatch was refused before the SDK call; the order was not sent");
  }
  if (result.code) {
    const auto reported = trader_->snapshot();
    if (std::ranges::none_of(reported.orders,
                             [&](const BrokerOrder& value) { return value.order_id == order_id; }))
      throw Error(
          ErrorCode::unavailable,
          "SDK did not confirm order dispatch; verify the broker before sending another order");
  }
}
void LiveAccountState::poll_broker() {
  trader_->poll();
  const auto state = trader_->snapshot();
  if (attribution_.generation != state.connection_generation ||
      attribution_.day != state.trading_day)
    attribution_ = state.phase == "ready"
                       ? Attribution{state.trading_day, state.connection_generation}
                       : Attribution{};
  if (!identities_ready(state))
    return;
  for (const auto& order : state.orders) {
    if (working(order.status))
      continue;
    const auto intent = intents_.find(order.order_id);
    if (intent != intents_.end() && !intent->second.terminal &&
        intent->second.trading_day == state.trading_day &&
        intent->second.broker_key == order.broker_key)
      intent->second.terminal = order;
  }
}
void LiveAccountState::validate_terminal(const Json& record, const Intent& intent) {
  require_fields(record, {"order_id", "broker_key", "trading_day", "status", "filled",
                          "exchange_order_id", "error_code"});
  const auto status = text(record, "status");
  const auto filled = Decimal::parse(text(record, "filled"));
  if (record.at("broker_key") != intent.broker_key ||
      record.at("trading_day") != intent.trading_day ||
      (status != "filled" && status != "cancelled" && status != "rejected") || filled < Decimal{} ||
      filled > intent.quantity || (status == "filled" && filled != intent.quantity) ||
      (status == "rejected" && filled != Decimal{}) ||
      !record.at("exchange_order_id").is_string() || !record.at("error_code").is_number_integer())
    throw std::invalid_argument("invalid persisted terminal order evidence");
}
bool LiveAccountState::storage_work_pending() const {
  return !failed_ && ((!attribution_.day.empty() && !attribution_.complete) ||
                      std::ranges::any_of(
                          intents_, [](const auto& entry) { return bool(entry.second.terminal); }));
}
AccountCommand LiveAccountState::advance_storage() {
  try {
    Json orders = Json::array();
    for (const auto& [id, intent] : intents_) {
      if (!intent.terminal)
        continue;
      const auto& order = *intent.terminal;
      Json record{{"order_id", id},
                  {"broker_key", intent.broker_key},
                  {"trading_day", intent.trading_day},
                  {"status", status_name(order.status)},
                  {"filled", order.filled.str()},
                  {"exchange_order_id", order.exchange_order_id},
                  {"error_code", order.error_code}};
      validate_terminal(record, intent);
      orders.push_back(std::move(record));
      if (orders.size() == terminal_batch)
        break;
    }
    if (!orders.empty()) {
      Json record{{"orders_terminal", orders}};
      co_await append(std::move(record));
      for (const auto& order : orders)
        intents_.erase(order.at("order_id").get<std::string>());
      co_return;
    }
    const auto scope = attribution_;
    auto result = co_await journal_->restore_orders(scope.day, scope.cursor);
    if (attribution_.day != scope.day || attribution_.generation != scope.generation ||
        !trader_->restore_orders(scope.day, scope.generation, result.orders))
      co_return;
    attribution_.cursor = result.order_cursor;
    attribution_.complete = result.orders.empty();
  } catch (...) {
    failed_ = true;
    send_gate_.invalidate();
    throw;
  }
}
std::chrono::steady_clock::time_point LiveAccountState::next_broker_deadline() const {
  return trader_->next_deadline();
}
void LiveAccountState::poll_sdk() {
  if (sdk_ready_ && sdk_ready_()) {
    sdk_ready_ = {};
    const auto command = std::exchange(sdk_continuation_, {});
    command.resume();
  }
}
bool LiveAccountState::business_ready() const {
  return !failed_ && attribution_.complete && trader_->ready();
}
bool LiveAccountState::identities_ready(const BrokerSnapshot& state) const {
  return attribution_.complete && attribution_.generation == state.connection_generation &&
         attribution_.day == state.trading_day;
}
bool LiveAccountState::authorized(const BrokerSnapshot& state) const {
  return !authorization_.is_null() && state.phase == "ready" && identities_ready(state) &&
         authorization_generation_ == state.connection_generation &&
         authorization_.at("trading_day") == state.trading_day;
}
Json LiveAccountState::snapshot() const {
  const auto state = trader_->snapshot();
  log_broker_observations(state);
  const auto capacity = capacity_;
  const auto definition = policy_->definition();
  Json result{
      {"broker",
       {{"broker_id", broker_id_},
        {"user_id", investor_id_},
        {"front", trade_front_},
        {"app_id", app_id_}}},
      {"policy_revision", policy_->revision},
      {"risk_artifact", policy_->algorithm.artifact()},
      {"account_id", header_.at("account_id")},
      {"segment_count", capacity.segment_count},
      {"risk", definition.at("risk")},
      {"max_price_deviation", definition.at("max_price_deviation")},
      {"contracts", definition.at("contracts")},
      {"phase", state.phase == "ready" && !identities_ready(state) ? "synchronizing" : state.phase},
      {"error_code", state.error_code},
      {"trading_day", state.trading_day},
      {"synchronized_ms", state.synchronized_ms},
      {"funds", nullptr},
      {"positions", Json::array()},
      {"orders", Json::array()},
      {"trades", Json::array()},
      {"authorization", authorized(state) ? authorization_ : Json(nullptr)},
      {"unconfirmed", Json::array()},
      {"costs", Json::array()},
      {"storage_state", failed_ ? "recovery_required" : "ready"},
      {"capacity",
       {{"records_used", capacity.records_used},
        {"records_limit", capacity.records_limit},
        {"bytes_used", capacity.bytes_used},
        {"bytes_limit", capacity.bytes_limit}}}};
  for (const auto& c : state.costs) {
    Json costs = nullptr;
    if (c.costs)
      costs = {{"margin_per_lot", c.costs->margin_per_lot.str()},
               {"open_fee", c.costs->open_fee.str()},
               {"close_today_fee", c.costs->close_today_fee.str()},
               {"close_yesterday_fee", c.costs->close_yesterday_fee.str()},
               {"margin_rate", c.costs->margin_rate.str()},
               {"open_fee_rate", c.costs->open_fee_rate.str()},
               {"close_today_fee_rate", c.costs->close_today_fee_rate.str()},
               {"close_yesterday_fee_rate", c.costs->close_yesterday_fee_rate.str()}};
    result["costs"].push_back({{"venue", c.instrument.venue},
                               {"symbol", c.instrument.symbol},
                               {"state", c.state},
                               {"error_code", c.error_code},
                               {"queried_ms", c.queried_ms},
                               {"costs", std::move(costs)}});
  }
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
