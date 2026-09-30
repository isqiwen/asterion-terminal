#include "paper_session.hpp"
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
// Journal record 0 carries this identity. Bump it whenever a change to
// matching, account/fee/margin arithmetic, risk evaluation or command
// semantics could make replaying an existing journal produce a different
// ledger. Recovery refuses a journal written under a different identity rather
// than silently recomputing history with new rules.
// v3: data-source bar replay, next-bar conservative fills, day-end settlement
// from the dataset's trading days.
const std::string journal_engine = "asterion.paper-futures.v4";
constexpr int journal_format = 4;
// Cheap fingerprint of post-command state. Replay must reproduce it exactly.
Json outcome(const PaperExecution& engine, const Json& authorization, const Json& replay) {
  const auto& account = engine.account();
  Decimal long_quantity, short_quantity;
  for (const auto& lot : account.positions()) {
    auto& total = lot.side == Side::buy ? long_quantity : short_quantity;
    total = total + lot.quantity;
  }
  return {{"cursor", engine.cursor()},         {"balance", account.balance().str()},
          {"fees", account.fees().str()},      {"realized", account.realized().str()},
          {"frozen", account.frozen().str()},  {"mark", account.last_mark().str()},
          {"orders", account.orders().size()}, {"fills", account.fills().size()},
          {"long", long_quantity.str()},       {"short", short_quantity.str()},
          {"authorization", authorization},    {"replay", replay}};
}
// No consumed events, orders or positions.
bool fresh(const PaperExecution& engine) {
  return engine.cursor() == 0 && engine.account().orders().empty() &&
         engine.account().positions().empty();
}
} // namespace
std::unique_ptr<PaperExecution> PaperSession::build(const Json& manifest) {
  const auto input = protocol::encode_input(manifest);
  const auto& c = input.dataset().contract();
  FuturesContract contract{protocol::instrument(c), c.product(), c.delivery_month()};
  contract.validate();
  auto risk = risk_module_->create(decode_order_limits(manifest.at("risk")));
  risk->start();
  return std::make_unique<PaperExecution>(contract.instrument,
                                          Decimal::from_raw(input.deposit().units()),
                                          protocol::futures_costs(input.costs()),
                                          protocol::dataset_bars(input.dataset()), std::move(risk));
}
void PaperSession::apply(PaperExecution& engine, Json& authorization, Json& replay,
                         std::shared_ptr<const PaperReplaySchedule>& schedule,
                         const Json& command) const {
  const auto action = string(command, "action");
  validate_id(string(command, "request_id"));
  const bool controlled = !authorization.is_null() && authorization.at("active") == true;
  if (action == "replay_days") {
    require_fields(command, {"request_id", "action"});
    if (controlled || schedule || !fresh(engine))
      throw std::invalid_argument("day-end settlement requires a fresh unowned account");
    const auto input = protocol::encode_input(manifest_);
    schedule = std::make_shared<PaperReplaySchedule>(
        protocol::instrument(input.dataset().contract()), protocol::dataset_bars(input.dataset()),
        protocol::dataset_days(input.dataset()));
    replay = {{"settled_days", 0}};
    return;
  }
  if (action == "replay_settle") {
    require_fields(command, {"request_id", "action", "day_index"});
    if (!schedule || !command.at("day_index").is_number_integer() ||
        command.at("day_index") != replay.at("settled_days"))
      throw std::invalid_argument("unexpected replay settlement day");
    const auto cursor = engine.cursor();
    if (!cursor || !schedule->event(cursor - 1).day_end ||
        schedule->event(cursor - 1).day != command.at("day_index").get<std::size_t>())
      throw std::invalid_argument("replay day has not completed");
    const auto& day = schedule->day(schedule->event(cursor - 1).day);
    engine.cancel_open_orders();
    if (cursor == schedule->size())
      engine.settle(day.settlement_price);
    else
      engine.settle_day_end(day.settlement_price);
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
        !maximum.multiple_of(decimal(manifest_.at("dataset").at("contract"), "quantity_increment")))
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
                             "dataset_revision", "sequence", "timestamp_ns", "target_quantity"});
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
    if (!schedule || !schedule->event(cursor - 1).day_end)
      engine.reconcile_long_target(string(command, "request_id"), target,
                                   engine.account().last_mark());
    authorization["last_sequence"] = command.at("sequence");
    return;
  }
  if (controlled && action != "advance")
    throw std::invalid_argument("revoke strategy authorization before manual account operations");
  if (action == "advance") {
    require_fields(command, {"request_id", "action"});
    const auto cursor = engine.cursor();
    if (schedule && cursor < schedule->size() &&
        schedule->event(cursor).day != replay.at("settled_days").get<std::size_t>())
      throw std::invalid_argument("settle the completed replay day before advancing");
    engine.advance();
    if (schedule && schedule->event(cursor).day_end)
      engine.cancel_open_orders();
  } else if (action == "cancel") {
    require_fields(command, {"request_id", "action", "order_id"});
    engine.cancel(string(command, "order_id"));
  } else if (action == "settle") {
    require_fields(command, {"request_id", "action", "price"});
    if (schedule)
      throw std::invalid_argument("scheduled replay requires its bound settlement price");
    engine.settle(decimal(command, "price"));
  } else if (action == "submit") {
    require_fields(command,
                   {"request_id", "action", "order_id", "side", "offset", "quantity", "price"});
    const auto cursor = engine.cursor();
    if (schedule && (!cursor || schedule->event(cursor - 1).day_end))
      throw std::invalid_argument("cannot submit after the last bar of a trading day");
    const auto side = string(command, "side"), offset = string(command, "offset");
    if (side != "buy" && side != "sell")
      throw std::invalid_argument("invalid order side");
    if (offset != "open" && offset != "close_today" && offset != "close_yesterday" &&
        offset != "close")
      throw std::invalid_argument("invalid open/close offset");
    validate_id(string(command, "order_id"));
    engine.submit({string(command, "order_id"), instrument_, side == "buy" ? Side::buy : Side::sell,
                   decimal(command, "quantity"), decimal(command, "price")},
                  offset == "open"          ? Offset::open
                  : offset == "close_today" ? Offset::close_today
                  : offset == "close"       ? Offset::close
                                            : Offset::close_yesterday);
  } else
    throw std::invalid_argument("unsupported paper trading operation");
}
PaperSession::PaperSession(std::filesystem::path directory, const Json& create_manifest)
    : journal_(directory, {"plugins"}) {
  if (std::filesystem::exists(directory / "pending.tmp") ||
      std::filesystem::is_symlink(directory / "pending.tmp"))
    throw std::invalid_argument("incomplete trading journal write; preserve it "
                                "for inspection before recovery");
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
    if (!header.is_object() || !header.contains("format") || header.at("format") != journal_format)
      throw std::invalid_argument("unsupported trading journal format; this build reads format 4 "
                                  "only and leaves the directory unchanged");
    require_fields(header, {"format", "engine", "risk_artifact", "manifest"});
    if (header.at("engine") != journal_engine)
      throw std::invalid_argument(
          "trading journal was written by engine " + header.at("engine").dump() +
          " but this build implements " + journal_engine +
          "; recovery refused instead of recomputing history under different rules");
    risk_module_ = risk_providers::Module::pinned(directory / "plugins",
                                                  header.at("risk_artifact").get<std::string>());
    manifest_ = header.at("manifest");
    engine_ = build(manifest_);
    engine_->start();
  }
  const auto& contract = manifest_.at("dataset").at("contract");
  dataset_revision_ = string(manifest_.at("dataset"), "revision");
  instrument_ = {string(contract, "venue"), string(contract, "symbol")};
  for (std::size_t i = 1; i < records.size(); ++i) {
    require_fields(records[i], {"command", "outcome"});
    const auto& command = records[i].at("command");
    const auto id = string(command, "request_id");
    if (commands_.contains(id))
      throw std::invalid_argument("trading journal contains a duplicate request; recovery refused");
    validate_history(command);
    apply(*engine_, authorization_, replay_, schedule_, command);
    if (outcome(*engine_, authorization_, replay_) != records[i].at("outcome"))
      throw std::invalid_argument("trading journal replay diverged from the recorded outcome at "
                                  "record " +
                                  std::to_string(i) + "; recovery refused");
    sequence_.push_back(&commands_.emplace(id, command).first->second);
  }
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
  Json authorization = nullptr, replay = nullptr;
  std::shared_ptr<const PaperReplaySchedule> schedule;
  for (const auto* command : sequence_)
    apply(*engine, authorization, replay, schedule, *command);
  engine_ = std::move(engine);
  authorization_ = std::move(authorization);
  replay_ = std::move(replay);
  schedule_ = std::move(schedule);
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
  auto schedule = schedule_;
  const auto before = engine_->revision();
  try {
    apply(*engine_, authorization, replay, schedule, command);
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
  schedule_.swap(schedule);
  sequence_.push_back(&commands_.insert(std::move(entry)).position->second);
}
Json PaperSession::snapshot() const {
  auto result = engine_->snapshot();
  result["contract"] = manifest_.at("dataset").at("contract");
  result["costs"] = manifest_.at("costs");
  result["risk"] = manifest_.at("risk");
  result["persistent"] = true;
  result["storage_state"] = failed_ ? "recovery_required" : "ready";
  if (!replay_.is_null()) {
    result["replay"] = replay_;
    const auto cursor = result.at("cursor").get<std::size_t>();
    result["replay"]["day_end"] = cursor && schedule_->event(cursor - 1).day_end;
    result["replay"]["settlement_due"] =
        cursor && schedule_->event(cursor - 1).day_end &&
        replay_.at("settled_days") == schedule_->event(cursor - 1).day;
  }
  if (!authorization_.is_null())
    result["strategy"] = authorization_;
  return result;
}
} // namespace asterion::trading
