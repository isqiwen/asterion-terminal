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
std::string text(const Json& value, const char* key) {
  auto result = value.at(key).get<std::string>();
  if (result.empty() || result.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid live trading field");
  return result;
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
  account_id_ = text(manifest, "account_id");
  const auto& account_id = account_id_;
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
  journal_->replay(
      [&](const Json& header) {
        if (header != header_)
          throw Error(ErrorCode::conflict,
                      "account record changed while acquiring execution ownership");
      },
      [&](const JournalRecord& record) {
        if (const auto* terminal = std::get_if<OrdersTerminal>(&record)) {
          for (const auto& order : terminal->orders) {
            const auto found = intents_.find(order.order_id);
            if (found == intents_.end())
              throw std::invalid_argument("invalid persisted terminal order evidence");
            check_terminal(order, found->second);
            intents_.erase(found);
          }
          return;
        }
        if (const auto* unsent = std::get_if<OrderNotSent>(&record)) {
          if (!intents_.erase(unsent->order_id))
            throw std::invalid_argument("invalid unsent live order result");
          return;
        }
        if (const auto* cancel = std::get_if<CancelResult>(&record)) {
          if (!pending_cancels.erase(cancel->request_id))
            throw std::invalid_argument("invalid live cancellation result");
          return;
        }
        const auto& command = std::get<CommandRecord>(record);
        const auto& request = command.request;
        if (command.policy_revision != revision)
          throw std::invalid_argument("trading command policy does not match the active revision");
        if (request.as<CancelOrder>())
          pending_cancels.insert(request.id);
        else if (const auto* change = request.as<ChangePolicy>()) {
          revision = command.new_policy_revision;
          if (!revisions.insert(revision).second)
            throw std::invalid_argument("account policy revision is repeated");
          definition =
              protocol::decode_live_policy(protocol::encode_live_policy(change->definition));
          artifact = command.risk_artifact;
        } else if (const auto* resolve = request.as<ResolveOrder>()) {
          if (!intents_.erase(resolve->order_id))
            throw std::invalid_argument("live trading record resolves an unknown order");
        } else if (const auto* submit = request.as<SubmitOrder>())
          intents_[submit->order.id] = {command.broker_key, command.trading_day,
                                        submit->order.instrument, submit->offset,
                                        submit->order.quantity};
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
AccountCommand LiveAccountState::append(JournalEntry entry) {
  const auto trace = entry.trace;
  try {
    capacity_ = (co_await journal_->append(std::move(entry))).capacity;
  } catch (...) {
    failed_ = true;
    throw;
  }
  log_identity_event("trading", "journal.committed",
                     {{"account_id", account_id_},
                      {"request_id", trace.request_id},
                      {"action", trace.action},
                      {"order_id", trace.order_id},
                      {"broker_key", trace.broker_key},
                      {"outcome", trace.outcome}});
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
    const auto& record = account_id_;
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
  authorization_.reset();
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
  authorization_.reset();
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
  if (state.phase != BrokerPhase::ready || !identities_ready(state))
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
  if (account_id.empty() || account_id != account_id_)
    throw Error(
        ErrorCode::conflict,
        "trading account identity does not match; review the current account before acting");
}
AccountCommand LiveAccountState::revise_policy(const AccountRequest& request,
                                               const ChangePolicy& change) {
  auto prepared = co_await journal_->prepare_policy(directory_ / "plugins", change.definition,
                                                    unique_process_id(), change.risk_artifact,
                                                    policy_->algorithm.artifact());
  auto next = std::move(prepared.policy);
  std::exception_ptr failure;
  try {
    const auto state = trader_->snapshot();
    if (state.phase != BrokerPhase::ready || !identities_ready(state) ||
        !state.positions_reconciled || !unconfirmed(state).empty())
      throw Error(ErrorCode::conflict,
                  "policy change requires reconciled positions and no unconfirmed orders");
    for (const auto& position : state.positions)
      if (position.today + position.yesterday > Decimal{})
        next->preserve_exposure(*policy_, position.instrument);
    for (const auto& order : state.orders)
      if (working(order.status))
        next->preserve_exposure(*policy_, order.instrument);
    trader_->disconnect_checked(state.connection_generation, state.exposure_revision);
    authorization_.reset();
    send_gate_.invalidate();
    co_await append(
        policy_entry(request, policy_->revision, next->revision, next->algorithm.artifact()));
    policy_.swap(next);
  } catch (...) {
    failure = std::current_exception();
  }
  // Both a rejected candidate and a replaced policy release their plugin on the writer.
  co_await journal_->retire_policy(std::move(next));
  if (failure)
    std::rethrow_exception(failure);
}
void LiveAccountState::admit_revoke(std::string_view account_id, std::string_view revision) {
  check_account(account_id);
  if (revision != policy_->revision)
    throw Error(ErrorCode::conflict,
                "account policy changed; review the current revision before acting");
  // The entry point already closed permission. Only the owner changes authorization.
  authorization_.reset();
}
AccountCommand LiveAccountState::execute(std::string account_id, std::string policy_revision,
                                         AccountRequest request, std::uint64_t admitted_control) {
  check_account(account_id);
  if (failed_)
    throw std::invalid_argument("live trading record needs recovery; reopen the session");
  std::optional<CommandRecord> recorded;
  try {
    recorded = std::move((co_await journal_->find_command(request.id)).command);
  } catch (...) {
    failed_ = true;
    throw;
  }
  if (recorded) {
    // A retried order or cancel is acknowledged, never sent again. An
    // authorization is never acknowledged from history: it may have ended.
    if (recorded->request.command != request.command ||
        recorded->policy_revision != policy_revision || request.as<Authorize>() ||
        request.as<Revoke>())
      throw Error(ErrorCode::conflict, "request ID was already used");
    co_return;
  }
  if (policy_revision != policy_->revision)
    throw Error(ErrorCode::conflict,
                "account policy changed; review the current revision before acting");
  if ((request.as<Authorize>() || request.as<SubmitOrder>()) &&
      admitted_control != send_gate_.revision())
    throw Error(ErrorCode::conflict,
                "account control changed after command admission; submit a new request");
  if (const auto* change = request.as<ChangePolicy>()) {
    co_await revise_policy(request, *change);
  } else if (const auto* grant = request.as<Authorize>()) {
    if (grant->user_id != investor_id_)
      throw std::invalid_argument("authorization names a different account");
    const auto state = trader_->snapshot();
    if (state.phase != BrokerPhase::ready || !identities_ready(state))
      throw Error(ErrorCode::unavailable, "connect and synchronize the account before authorizing");
    Authorization authorization{state.trading_day, now_ms()};
    co_await append(authorization_entry(request, policy_->revision, authorization));
    if (admitted_control != send_gate_.revision() ||
        state.connection_generation !=
            observed([](const BrokerSnapshot& now) { return now.connection_generation; }))
      throw Error(ErrorCode::conflict, "account state changed before durable grant completed");
    authorization_ = std::move(authorization);
    authorization_generation_ = state.connection_generation;
  } else if (request.as<Revoke>()) {
    co_await append(command_entry(request, policy_->revision));
  } else if (const auto* resolve = request.as<ResolveOrder>()) {
    // Only an order listed as unconfirmed right now: the broker is
    // synchronized and does not report it.
    const auto listed = observed([&](const BrokerSnapshot& state) { return unconfirmed(state); });
    if (std::ranges::none_of(listed,
                             [&](const auto& item) { return item.first == resolve->order_id; }))
      throw std::invalid_argument("only an unconfirmed order of a synchronized account can be "
                                  "resolved");
    co_await append(command_entry(request, policy_->revision));
    intents_.erase(resolve->order_id);
  } else if (const auto* order = request.as<SubmitOrder>()) {
    co_await submit(request, *order, admitted_control);
  } else {
    const auto& order_id = std::get<CancelOrder>(request.operation).order_id;
    if (!observed([&](const BrokerSnapshot& state) { return identities_ready(state); }))
      throw Error(ErrorCode::unavailable, "account identity reconciliation is not complete");
    if (!intents_.contains(order_id))
      throw Error(ErrorCode::conflict, "order is not active in this account");
    co_await append(command_entry(request, policy_->revision));
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
    co_await append(entry(CancelResult{request.id, !failure}));
    if (failure)
      std::rethrow_exception(failure);
  }
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
AccountCommand LiveAccountState::submit(const AccountRequest& request,
                                        const SubmitOrder& submission, std::uint64_t control) {
  const auto& order = submission.order;
  const auto& order_id = order.id;
  const auto& id = order.instrument;
  const auto offset = submission.offset;
  bool known = false;
  try {
    known = (co_await journal_->find_order(order_id)).order_known;
  } catch (...) {
    failed_ = true;
    throw;
  }
  if (known)
    throw Error(ErrorCode::conflict, "order ID was already used");
  const auto [permitted, reconciled] = observed([&](const BrokerSnapshot& state) {
    return std::pair{authorized(state), state.positions_reconciled};
  });
  if (!permitted)
    throw std::invalid_argument("authorize live trading for this account first");
  const auto& terms = allowed(id);
  if (offset == Offset::open && !reconciled)
    throw Error(ErrorCode::unavailable,
                "broker fills are awaiting position reconciliation; opening orders are paused");
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
  trader_->observe([&](const BrokerSnapshot& now) { check_price(order, quote, now); });
  co_await append(
      submission_entry(request, policy_->revision, prepared_order.broker_key, state.trading_day));
  const auto sequence = capacity_.total_records - 1;
  intents_[order_id] = {prepared_order.broker_key, state.trading_day, id, offset, order.quantity};
  // Only positive local evidence can release the recorded intent. If recording
  // it fails, recovery retains the intent as an unknown order.
  const auto not_sent = [&](int code) -> AccountCommand {
    co_await append(entry(OrderNotSent{order_id, code}));
    intents_.erase(order_id);
  };
  if (control != send_gate_.revision() ||
      !observed([&](const BrokerSnapshot& now) { return authorized(now); })) {
    co_await not_sent(broker_code::permit_refused);
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
    co_await not_sent(broker_code::permit_refused);
    throw Error(ErrorCode::conflict,
                "order permission changed before dispatch; the order was not sent");
  }
  const auto result =
      co_await wait_sdk(trader_->dispatch(std::move(prepared), std::move(*permit), sequence));
  if (!result.invoked) {
    co_await not_sent(result.code);
    throw Error(ErrorCode::conflict,
                "order dispatch was refused before the SDK call; the order was not sent");
  }
  if (result.code) {
    if (!observed([&](const BrokerSnapshot& now) {
          return std::ranges::any_of(
              now.orders, [&](const BrokerOrder& value) { return value.order_id == order_id; });
        }))
      throw Error(
          ErrorCode::unavailable,
          "SDK did not confirm order dispatch; verify the broker before sending another order");
  }
}
void LiveAccountState::poll_broker() {
  trader_->poll();
  trader_->observe([&](const BrokerSnapshot& state) {
    if (attribution_.generation != state.connection_generation ||
        attribution_.day != state.trading_day)
      attribution_ = state.phase == BrokerPhase::ready
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
  });
}
void LiveAccountState::check_terminal(const TerminalOrder& order, const Intent& intent) {
  const bool final = order.status == BrokerOrderStatus::filled ||
                     order.status == BrokerOrderStatus::cancelled ||
                     order.status == BrokerOrderStatus::rejected;
  if (order.broker_key != intent.broker_key || order.trading_day != intent.trading_day || !final ||
      order.filled < Decimal{} || order.filled > intent.quantity ||
      (order.status == BrokerOrderStatus::filled && order.filled != intent.quantity) ||
      (order.status == BrokerOrderStatus::rejected && order.filled != Decimal{}))
    throw std::invalid_argument("invalid persisted terminal order evidence");
}
bool LiveAccountState::storage_work_pending() const {
  return !failed_ && ((!attribution_.day.empty() && !attribution_.complete) ||
                      std::ranges::any_of(
                          intents_, [](const auto& entry) { return bool(entry.second.terminal); }));
}
AccountCommand LiveAccountState::advance_storage() {
  try {
    OrdersTerminal terminal;
    for (const auto& [id, intent] : intents_) {
      if (!intent.terminal)
        continue;
      const auto& order = *intent.terminal;
      TerminalOrder evidence{
          id,           intent.broker_key,       intent.trading_day, order.status,
          order.filled, order.exchange_order_id, order.error_code};
      check_terminal(evidence, intent);
      terminal.orders.push_back(std::move(evidence));
      if (terminal.orders.size() == OrdersTerminal::batch)
        break;
    }
    if (!terminal.orders.empty()) {
      co_await append(entry(terminal));
      for (const auto& order : terminal.orders)
        intents_.erase(order.order_id);
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
  return !failed_ && attribution_.complete && observed([](const BrokerSnapshot& state) {
    return state.phase == BrokerPhase::ready && state.positions_reconciled;
  });
}
bool LiveAccountState::identities_ready(const BrokerSnapshot& state) const {
  return attribution_.complete && attribution_.generation == state.connection_generation &&
         attribution_.day == state.trading_day;
}
bool LiveAccountState::authorized(const BrokerSnapshot& state) const {
  return authorization_ && state.phase == BrokerPhase::ready && identities_ready(state) &&
         authorization_generation_ == state.connection_generation &&
         authorization_->trading_day == state.trading_day;
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
      {"account_id", account_id_},
      {"segment_count", capacity.segment_count},
      {"risk", definition.at("risk")},
      {"max_price_deviation", definition.at("max_price_deviation")},
      {"contracts", definition.at("contracts")},
      {"phase", broker_phase_name(state.phase == BrokerPhase::ready && !identities_ready(state)
                                      ? BrokerPhase::synchronizing
                                      : state.phase)},
      {"error_code", state.error_code},
      {"trading_day", state.trading_day},
      {"synchronized_ms", state.synchronized_ms},
      {"funds", nullptr},
      {"positions", Json::array()},
      {"orders", Json::array()},
      {"trades", Json::array()},
      {"authorization", authorized(state) ? authorization_->json() : Json(nullptr)},
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
                               {"state", broker_costs_state_name(c.state)},
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
