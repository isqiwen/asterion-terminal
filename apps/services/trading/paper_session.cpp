#include "paper_session.hpp"
#include "paper_record.hpp"
#include "portfolio.hpp"
#include <algorithm>
#include "order_limits.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/protocol/data.hpp>
#include <type_traits>
#include <stdexcept>
namespace asterion::trading {
namespace {
std::string string(const Json& value, const char* key) {
  auto result = value.at(key).get<std::string>();
  if (result.empty() || result.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid paper trading field");
  return result;
}
Decimal decimal(const Json& value, const char* key) {
  return Decimal::parse(string(value, key));
}
// Cheap fingerprint of post-command state. Replay must reproduce it exactly.
Json outcome(const PaperExecution& engine, const Json& authorization, const Json& replay) {
  const auto& account = engine.account();
  Json contracts = Json::array();
  for (const auto& terms : account.contracts()) {
    Decimal long_quantity, short_quantity;
    for (const auto& lot : account.positions())
      if (lot.instrument == terms.instrument.id) {
        auto& total = lot.side == Side::buy ? long_quantity : short_quantity;
        total = total + lot.quantity;
      }
    contracts.push_back({{"mark", account.last_mark(terms.instrument.id).str()},
                         {"long", long_quantity.str()},
                         {"short", short_quantity.str()}});
  }
  return {{"cursor", engine.cursor()},        {"balance", account.balance().str()},
          {"fees", account.fees().str()},     {"realized", account.realized().str()},
          {"frozen", account.frozen().str()}, {"orders", account.orders().size()},
          {"fills", account.fills().size()},  {"contracts", std::move(contracts)},
          {"authorization", authorization},   {"replay", replay}};
}
// No consumed events, orders or positions.
bool fresh(const PaperExecution& engine) {
  return engine.cursor() == 0 && engine.account().orders().empty() &&
         engine.account().positions().empty();
}
} // namespace
std::unique_ptr<PaperExecution> PaperSession::build(const Json& manifest) {
  const auto input = protocol::encode_input(manifest);
  std::size_t bars = 0;
  for (const auto& contract : input.contracts()) {
    const auto& c = contract.dataset().contract();
    FuturesContract{protocol::instrument(c), c.product(), c.delivery_month()}.validate();
    bars += static_cast<std::size_t>(contract.dataset().bars_size());
  }
  if (bars > protocol::max_session_bars)
    throw std::invalid_argument("paper sessions use at most 20000 bars; narrow the trading days");
  auto risk = risk_module_->create(decode_order_limits(manifest.at("risk")));
  risk->start();
  auto portfolio = paper_portfolio(input);
  schedule_ = std::make_shared<const PaperReplaySchedule>(replay_schedule(portfolio));
  return std::make_unique<PaperExecution>(Decimal::from_raw(input.deposit().units()),
                                          std::move(portfolio.contracts), std::move(risk));
}
void PaperSession::apply(PaperExecution& engine, Json& authorization, Json& replay,
                         const Json& command) const {
  const auto& schedule = schedule_;
  const auto action = string(command, "action");
  validate_id(string(command, "request_id"));
  const bool controlled = !authorization.is_null() && authorization.at("active") == true;
  if (action == "replay_settle") {
    require_fields(command, {"request_id", "action", "day_index"});
    if (!command.at("day_index").is_number_integer() ||
        command.at("day_index") != replay.at("settled_days"))
      throw std::invalid_argument("unexpected replay settlement day");
    const auto cursor = engine.cursor();
    if (!cursor || !schedule->event(cursor - 1).day_end ||
        schedule->event(cursor - 1).day != command.at("day_index").get<std::size_t>())
      throw std::invalid_argument("replay day has not completed");
    const auto& day = schedule->day(schedule->event(cursor - 1).day);
    engine.cancel_open_orders();
    if (cursor == schedule->size())
      engine.settle(day.prices);
    else
      engine.settle_day_end(day.prices);
    replay["settled_days"] = command.at("day_index").get<std::size_t>() + 1;
    return;
  }
  if (action == "strategy_grant") {
    require_fields(command, {"request_id", "action", "grant_id", "strategy_id", "stream_id",
                             "dataset_revision", "max_quantity"});
    for (const auto* field : {"grant_id", "strategy_id", "stream_id"})
      validate_id(string(command, field));
    if (controlled || !fresh(engine))
      throw std::invalid_argument("strategy grant requires an unowned fresh account");
    const auto maximum = decimal(command, "max_quantity");
    if (command.at("dataset_revision") != dataset_revision_ || maximum <= Decimal{} ||
        std::ranges::any_of(engine.account().contracts(), [&](const ContractTerms& terms) {
          return !maximum.multiple_of(terms.instrument.quantity_increment);
        }))
      throw std::invalid_argument("invalid strategy dataset or position limit");
    authorization = {{"grant_id", command.at("grant_id")},
                     {"strategy_id", command.at("strategy_id")},
                     {"stream_id", command.at("stream_id")},
                     {"dataset_revision", dataset_revision_},
                     {"max_quantity", maximum.str()},
                     {"active", true},
                     {"last_sequence", 0}};
    return;
  }
  if (action == "strategy_revoke") {
    require_fields(command, {"request_id", "action", "grant_id"});
    if (!controlled || command.at("grant_id") != authorization.at("grant_id"))
      throw std::invalid_argument("strategy authorization is not active");
    engine.cancel_open_orders();
    authorization["active"] = false;
    return;
  }
  if (action == "strategy_target") {
    require_fields(command, {"request_id", "action", "grant_id", "strategy_id", "stream_id",
                             "dataset_revision", "sequence", "timestamp_ns", "venue", "symbol",
                             "target_quantity"});
    if (!controlled)
      throw std::invalid_argument("strategy authorization is not active");
    for (const auto* field : {"grant_id", "strategy_id", "stream_id", "dataset_revision"})
      if (command.at(field) != authorization.at(field))
        throw std::invalid_argument("strategy authorization mismatch");
    const auto cursor = engine.cursor();
    const auto timestamp = engine.timestamp_ns();
    if (!command.at("sequence").is_number_integer() || command.at("sequence") < 1 ||
        command.at("sequence") != cursor ||
        command.at("sequence") <= authorization.at("last_sequence") ||
        command.at("timestamp_ns") !=
            (timestamp ? Json(std::to_string(*timestamp)) : Json(nullptr)))
      throw std::invalid_argument("strategy intent is not for the current unconsumed event");
    const auto target = decimal(command, "target_quantity");
    if (target < Decimal{} || target > decimal(authorization, "max_quantity"))
      throw std::invalid_argument("strategy target exceeds authorized position limit");
    // The intent belongs to the contract of the event it observed.
    const InstrumentId instrument{string(command, "venue"), string(command, "symbol")};
    const auto& observed = engine.contract(engine.event(cursor - 1).contract).terms.instrument.id;
    if (instrument != observed)
      throw std::invalid_argument("strategy intent is not for the current event's contract");
    if (!schedule->event(cursor - 1).day_end)
      engine.reconcile_long_target(string(command, "request_id"), instrument, target,
                                   engine.account().last_mark(instrument));
    authorization["last_sequence"] = command.at("sequence");
    return;
  }
  if (controlled && action != "advance")
    throw std::invalid_argument("revoke strategy authorization before manual account operations");
  if (action == "advance") {
    require_fields(command, {"request_id", "action"});
    const auto cursor = engine.cursor();
    if (cursor < schedule->size() &&
        schedule->event(cursor).day != replay.at("settled_days").get<std::size_t>())
      throw std::invalid_argument("settle the completed replay day before advancing");
    engine.advance();
    if (schedule->event(cursor).day_end)
      engine.cancel_open_orders();
  } else if (action == "cancel") {
    require_fields(command, {"request_id", "action", "order_id"});
    engine.cancel(string(command, "order_id"));
  } else if (action == "submit") {
    require_fields(command, {"request_id", "action", "order_id", "venue", "symbol", "side",
                             "offset", "quantity", "price"});
    const auto cursor = engine.cursor();
    if (!cursor || schedule->event(cursor - 1).day_end)
      throw std::invalid_argument("cannot submit after the last bar of a trading day");
    const auto side = string(command, "side"), offset = string(command, "offset");
    if (side != "buy" && side != "sell")
      throw std::invalid_argument("invalid order side");
    if (offset != "open" && offset != "close_today" && offset != "close_yesterday" &&
        offset != "close")
      throw std::invalid_argument("invalid open/close offset");
    validate_id(string(command, "order_id"));
    engine.submit({string(command, "order_id"),
                   {string(command, "venue"), string(command, "symbol")},
                   side == "buy" ? Side::buy : Side::sell,
                   decimal(command, "quantity"),
                   decimal(command, "price")},
                  offset == "open"          ? Offset::open
                  : offset == "close_today" ? Offset::close_today
                  : offset == "close"       ? Offset::close
                                            : Offset::close_yesterday);
  } else
    throw std::invalid_argument("unsupported paper trading operation");
}
PaperSession::PaperSession(std::filesystem::path directory, const Json& create_manifest)
    : journal_(directory, {"plugins"}) {
  journal_.start();
  auto records = journal_.read();
  if (!create_manifest.is_null()) {
    if (!records.empty())
      throw std::invalid_argument("directory already holds a session; recover it instead");
    risk_module_ = risk_providers::Module::selected();
    manifest_ = create_manifest;
    engine_ = build(manifest_);
    engine_->start();
    if (!std::filesystem::create_directory(directory / "plugins"))
      throw std::invalid_argument("risk plugin snapshot already exists or directory is invalid");
    risk_module_->capture(directory / "plugins");
    journal_.append({{"format", journal_format},
                     {"engine", journal_engine},
                     {"risk_artifact", risk_module_->artifact()},
                     {"manifest", manifest_}});
  } else {
    if (records.empty())
      throw std::invalid_argument("directory holds no recoverable paper session");
    const auto& header = records.front();
    validate_paper_header(header);
    risk_module_ = risk_providers::Module::pinned(directory / "plugins",
                                                  header.at("risk_artifact").get<std::string>());
    manifest_ = header.at("manifest");
    engine_ = build(manifest_);
    engine_->start();
  }
  const auto input = protocol::encode_input(manifest_);
  dataset_revision_ = protocol::dataset_revision(input);
  for (const auto& contract : input.contracts()) {
    for (const auto& id : contract.dataset().source_dataset_ids())
      history_roles_[id].first = true;
    for (const auto& id : contract.dataset().settlement_dataset_ids())
      history_roles_[id].second = true;
  }
  replay_ = {{"settled_days", 0}};
  for (std::size_t i = 1; i < records.size(); ++i) {
    require_fields(records[i], {"command", "outcome"});
    const auto& command = records[i].at("command");
    const auto id = string(command, "request_id");
    if (commands_.contains(id))
      throw std::invalid_argument("trading journal contains a duplicate request; recovery refused");
    validate_history(command);
    apply(*engine_, authorization_, replay_, command);
    if (outcome(*engine_, authorization_, replay_) != records[i].at("outcome"))
      throw std::invalid_argument("trading journal replay diverged from the recorded outcome at "
                                  "record " +
                                  std::to_string(i) + "; recovery refused");
    sequence_.push_back(&commands_.emplace(id, command).first->second);
  }
}
protocol::v1::PaperHistoryUsage PaperSession::history_usage(const std::string& id) const {
  if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid historical dataset identity");
  protocol::v1::PaperHistoryUsage result;
  result.set_dataset_id(id);
  result.set_dataset_revision(dataset_revision_);
  if (const auto found = history_roles_.find(id); found != history_roles_.end()) {
    result.set_market(found->second.first);
    result.set_settlement(found->second.second);
  }
  return result;
}
void PaperSession::validate_history(const Json& command) const {
  if (string(command, "action") != "strategy_grant")
    return;
  for (const auto* previous : sequence_)
    if (previous->at("action") == "strategy_grant" &&
        previous->at("grant_id") == command.at("grant_id"))
      throw std::invalid_argument("strategy grant identity cannot be reused");
}
void PaperSession::restore() {
  auto engine = build(manifest_);
  engine->start();
  Json authorization = nullptr, replay = {{"settled_days", 0}};
  for (const auto* command : sequence_)
    apply(*engine, authorization, replay, *command);
  engine_ = std::move(engine);
  authorization_ = std::move(authorization);
  replay_ = std::move(replay);
}
PaperSession::~PaperSession() {
  if (engine_)
    engine_->stop();
}
void PaperSession::execute(const Json& command) {
  if (failed_)
    throw std::runtime_error("commit outcome unknown; close and reopen the session to recover");
  const auto id = string(command, "request_id");
  if (auto it = commands_.find(id); it != commands_.end()) {
    if (it->second != command)
      throw std::invalid_argument("request identity reused by a different operation");
    return;
  }
  validate_history(command);
  // Allocate tracking state before mutating the engine. Publication after the
  // durable commit only splices a map node and appends into reserved capacity.
  std::map<std::string, Json> staging;
  staging.emplace(id, command);
  auto entry = staging.extract(staging.begin());
  sequence_.reserve(sequence_.size() + 1);
  // Execute in place. Every engine operation is strongly exception safe, so a
  // rejected command with an unchanged engine revision needs no rollback; a
  // multi-step command that failed part way, or a failed commit, rebuilds the
  // engine from the committed sequence instead of copying the whole ledger
  // before every command.
  auto authorization = authorization_;
  auto replay = replay_;
  const auto before = engine_->revision();
  try {
    apply(*engine_, authorization, replay, command);
  } catch (...) {
    if (engine_->revision() != before) {
      try {
        restore();
      } catch (...) {
        failed_ = true;
      }
    }
    throw;
  }
  try {
    journal_.append({{"command", command}, {"outcome", outcome(*engine_, authorization, replay)}});
  } catch (...) {
    // The commit outcome is unknown; show the last committed state and refuse
    // further writes until the session is reopened and recovered.
    failed_ = true;
    try {
      restore();
    } catch (...) {
    }
    throw;
  }
  authorization_.swap(authorization);
  replay_.swap(replay);
  sequence_.push_back(&commands_.insert(std::move(entry)).position->second);
}
Json PaperSession::snapshot() const {
  auto result = engine_->snapshot();
  const auto& marks = result.at("marks");
  Json contracts = Json::array();
  for (std::size_t i = 0; i < manifest_.at("contracts").size(); ++i) {
    const auto& item = manifest_.at("contracts").at(i);
    contracts.push_back({{"contract", item.at("dataset").at("contract")},
                         {"costs", protocol::decode_costs(protocol::encode_costs(
                                       engine_->account().contracts()[i].costs))},
                         {"cost_schedule", item.at("cost_schedule")},
                         {"mark", marks.at(i).at("mark")}});
  }
  result.erase("marks");
  result["contracts"] = std::move(contracts);
  result["risk"] = manifest_.at("risk");
  result["persistent"] = true;
  result["storage_state"] = failed_ ? "recovery_required" : "ready";
  result["replay"] = replay_;
  const auto cursor = result.at("cursor").get<std::size_t>();
  result["replay"]["days"] = schedule_->days();
  result["replay"]["day_end"] = cursor && schedule_->event(cursor - 1).day_end;
  result["replay"]["settlement_due"] =
      cursor && schedule_->event(cursor - 1).day_end &&
      replay_.at("settled_days") == schedule_->event(cursor - 1).day;
  if (!authorization_.is_null())
    result["strategy"] = authorization_;
  return result;
}
} // namespace asterion::trading
