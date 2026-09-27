#include "paper_session.hpp"
#include "order_limits.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/protocol/data.hpp>
#include <charconv>
namespace asterion::trading {
namespace {
std::string string(const Json &value, const char *key) {
  auto result = value.at(key).get<std::string>();
  if (result.empty() || result.find('\0') != std::string::npos)
    throw std::invalid_argument("无效模拟交易字段");
  return result;
}
Decimal decimal(const Json &value, const char *key) {
  return Decimal::parse(string(value, key));
}
} // namespace
std::unique_ptr<PaperExecution> PaperSession::build(const Json &manifest) {
  require_fields(manifest, {"version", "type", "contract", "costs", "deposit",
                            "ticks", "risk"});
  if (!manifest.at("version").is_number_integer() ||
      manifest.at("version") != 1 || manifest.at("type") != "historical_paper")
    throw std::invalid_argument("不支持的模拟会话版本");
  const auto &c = manifest.at("contract");
  require_fields(c, {"venue", "symbol", "currency", "price_increment",
                     "quantity_increment", "multiplier", "product",
                     "delivery_month"});
  FuturesContract contract{{{string(c, "venue"), string(c, "symbol")},
                            AssetClass::futures,
                            string(c, "currency"),
                            decimal(c, "price_increment"),
                            decimal(c, "quantity_increment"),
                            decimal(c, "multiplier")},
                           string(c, "product"),
                           string(c, "delivery_month")};
  contract.validate();
  const auto &costs = manifest.at("costs");
  require_fields(costs, {"margin_per_lot", "open_fee", "close_today_fee",
                         "close_yesterday_fee"});
  const auto &rows = manifest.at("ticks");
  if (!rows.is_array() || rows.empty() || rows.size() > 10000)
    throw std::invalid_argument("模拟回放需要 1 至 10000 笔历史成交");
  std::vector<TradeTick> ticks;
  for (const auto &row : rows) {
    require_fields(row, {"timestamp_ns", "price", "quantity"});
    const auto value = string(row, "timestamp_ns");
    std::int64_t ns = 0;
    auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), ns);
    if (error != std::errc{} || end != value.data() + value.size() ||
        std::to_string(ns) != value)
      throw std::invalid_argument("无效历史成交时间");
    ticks.push_back({contract.instrument.id, ns, decimal(row, "price"),
                     decimal(row, "quantity")});
  }
  auto risk =
      std::make_shared<OrderLimits>(decode_order_limits(manifest.at("risk")));
  risk->start();
  return std::make_unique<PaperExecution>(
      contract.instrument, decimal(manifest, "deposit"),
      FuturesCosts{decimal(costs, "margin_per_lot"), decimal(costs, "open_fee"),
                   decimal(costs, "close_today_fee"),
                   decimal(costs, "close_yesterday_fee")},
      std::move(ticks), std::move(risk));
}
void PaperSession::apply(PaperExecution &engine, Json &authorization,
                         Json &replay,
                         std::shared_ptr<const PaperReplaySchedule> &schedule,
                         const Json &command) const {
  const auto action = string(command, "action");
  validate_id(string(command, "request_id"));
  const bool controlled =
      !authorization.is_null() && authorization.at("active") == true;
  if (action == "replay_calendar") {
    require_fields(command, {"request_id", "action", "publication"});
    const auto state = engine.snapshot();
    if (controlled || schedule || state.at("cursor") != 0 ||
        !state.at("orders").empty() || !state.at("positions").empty())
      throw std::invalid_argument(
          "calendar binding requires a fresh unowned account");
    const auto publication =
        protocol::encode_calendar_publication(command.at("publication"));
    if (protocol::decode_contract(publication.calendar().contract()) !=
        manifest_.at("contract"))
      throw std::invalid_argument(
          "calendar contract does not match trading account");
    const auto &c = publication.calendar().contract();
    const Instrument instrument{
        {c.venue(), c.symbol()},
        AssetClass::futures,
        c.currency(),
        Decimal::from_raw(c.price_increment().units()),
        Decimal::from_raw(c.quantity_increment().units()),
        Decimal::from_raw(c.multiplier().units())};
    std::vector<TradeTick> ticks;
    const auto input = protocol::encode_input(manifest_);
    for (const auto &tick : input.ticks())
      ticks.push_back({instrument.id, tick.timestamp_ns(),
                       Decimal::from_raw(tick.price().units()),
                       Decimal::from_raw(tick.quantity().units())});
    std::vector<SettlementDay> days;
    for (const auto &day : publication.calendar().days()) {
      std::vector<TradingSession> sessions;
      for (const auto &session : day.sessions())
        sessions.push_back({session.begin_ns(), session.end_ns()});
      days.push_back(
          {TradingDaySchedule(day.trading_day(), std::move(sessions)),
           Decimal::from_raw(day.settlement_price().units()),
           day.schedule_source(), day.settlement_source()});
    }
    schedule = std::make_shared<PaperReplaySchedule>(instrument, ticks,
                                                     std::move(days));
    replay = {
        {"publication", protocol::decode_calendar_publication(publication)},
        {"settled_days", 0}};
    return;
  }
  if (action == "replay_settle") {
    require_fields(command, {"request_id", "action", "day_index"});
    if (!schedule || !command.at("day_index").is_number_integer() ||
        command.at("day_index") != replay.at("settled_days"))
      throw std::invalid_argument("unexpected replay settlement day");
    const auto cursor = engine.snapshot().at("cursor").get<std::size_t>();
    if (!cursor || !schedule->event(cursor - 1).day_end ||
        schedule->event(cursor - 1).day !=
            command.at("day_index").get<std::size_t>())
      throw std::invalid_argument("replay day has not completed");
    const auto &day = schedule->day(schedule->event(cursor - 1).day);
    engine.cancel_open_orders();
    if (cursor == schedule->size())
      engine.settle(day.settlement_price);
    else
      engine.settle_before_next(day.schedule.sessions().back().end_ns,
                                day.settlement_price);
    replay["settled_days"] = command.at("day_index").get<std::size_t>() + 1;
    return;
  }
  if (action == "strategy_grant") {
    require_fields(command, {"request_id", "action", "grant_id", "strategy_id",
                             "stream_id", "dataset_revision", "max_quantity"});
    for (const auto *field : {"grant_id", "strategy_id", "stream_id"})
      validate_id(string(command, field));
    const auto state = engine.snapshot();
    if (controlled || state.at("cursor") != 0 ||
        !state.at("positions").empty() || !state.at("orders").empty())
      throw std::invalid_argument(
          "strategy grant requires an unowned fresh account");
    for (const auto &[id, previous] : commands_) {
      (void)id;
      if (previous.at("action") == "strategy_grant" &&
          previous.at("grant_id") == command.at("grant_id"))
        throw std::invalid_argument("strategy grant identity cannot be reused");
    }
    const auto maximum = decimal(command, "max_quantity");
    if (command.at("dataset_revision") != dataset_revision_ ||
        maximum <= Decimal{} ||
        !maximum.multiple_of(
            decimal(manifest_.at("contract"), "quantity_increment")))
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
    require_fields(command, {"request_id", "action", "grant_id", "strategy_id",
                             "stream_id", "dataset_revision", "sequence",
                             "timestamp_ns", "target_quantity"});
    if (!controlled)
      throw std::invalid_argument("strategy authorization is not active");
    for (const auto *field :
         {"grant_id", "strategy_id", "stream_id", "dataset_revision"})
      if (command.at(field) != authorization.at(field))
        throw std::invalid_argument("strategy authorization mismatch");
    const auto state = engine.snapshot();
    if (!command.at("sequence").is_number_integer() ||
        command.at("sequence") < 1 ||
        command.at("sequence") != state.at("cursor") ||
        command.at("sequence") <= authorization.at("last_sequence") ||
        command.at("timestamp_ns") != state.at("timestamp_ns"))
      throw std::invalid_argument(
          "strategy intent is not for the current unconsumed event");
    const auto target = decimal(command, "target_quantity");
    if (target < Decimal{} || target > decimal(authorization, "max_quantity"))
      throw std::invalid_argument(
          "strategy target exceeds authorized position limit");
    if (!schedule ||
        !schedule->event(state.at("cursor").get<std::size_t>() - 1).session_end)
      engine.reconcile_long_target(string(command, "request_id"), target,
                                   decimal(state, "mark"));
    authorization["last_sequence"] = command.at("sequence");
    return;
  }
  if (controlled && action != "advance")
    throw std::invalid_argument(
        "revoke strategy authorization before manual account operations");
  if (action == "advance") {
    require_fields(command, {"request_id", "action"});
    const auto cursor = engine.snapshot().at("cursor").get<std::size_t>();
    if (schedule && cursor < schedule->size() &&
        schedule->event(cursor).day !=
            replay.at("settled_days").get<std::size_t>())
      throw std::invalid_argument(
          "settle the completed replay day before advancing");
    engine.advance();
    if (schedule && schedule->event(cursor).session_end)
      engine.cancel_open_orders();
  } else if (action == "cancel") {
    require_fields(command, {"request_id", "action", "order_id"});
    engine.cancel(string(command, "order_id"));
  } else if (action == "settle") {
    require_fields(command, {"request_id", "action", "price"});
    if (schedule)
      throw std::invalid_argument(
          "scheduled replay requires its bound settlement price");
    engine.settle(decimal(command, "price"));
  } else if (action == "submit") {
    require_fields(command, {"request_id", "action", "order_id", "side",
                             "offset", "quantity", "price"});
    const auto cursor = engine.snapshot().at("cursor").get<std::size_t>();
    if (schedule && (!cursor || schedule->event(cursor - 1).session_end))
      throw std::invalid_argument(
          "cannot submit after the last replay event of a session");
    const auto side = string(command, "side"),
               offset = string(command, "offset");
    if (side != "buy" && side != "sell")
      throw std::invalid_argument("无效买卖方向");
    if (offset != "open" && offset != "close_today" &&
        offset != "close_yesterday")
      throw std::invalid_argument("无效开平标志");
    validate_id(string(command, "order_id"));
    engine.submit({string(command, "order_id"), instrument_,
                   side == "buy" ? Side::buy : Side::sell,
                   decimal(command, "quantity"), decimal(command, "price")},
                  offset == "open"          ? Offset::open
                  : offset == "close_today" ? Offset::close_today
                                            : Offset::close_yesterday);
  } else
    throw std::invalid_argument("不支持的模拟交易操作");
}
PaperSession::PaperSession(std::filesystem::path directory,
                           const Json &create_manifest)
    : journal_(directory) {
  if (std::filesystem::exists(directory / "pending.tmp") ||
      std::filesystem::is_symlink(directory / "pending.tmp"))
    throw std::invalid_argument("incomplete trading journal write; preserve it "
                                "for inspection before recovery");
  journal_.start();
  auto records = journal_.read();
  if (!create_manifest.is_null()) {
    if (!records.empty())
      throw std::invalid_argument("交易目录已有会话，请使用恢复功能");
    manifest_ = create_manifest;
    engine_ = build(manifest_);
    engine_->start();
    journal_.append(manifest_);
  } else {
    if (records.empty())
      throw std::invalid_argument("目录中没有可恢复的模拟会话");
    manifest_ = records.front();
    engine_ = build(manifest_);
    engine_->start();
  }
  const auto input = protocol::encode_input(manifest_);
  dataset_revision_ =
      protocol::make_trade_dataset(input.contract(), input.ticks()).revision();
  instrument_ = {string(manifest_.at("contract"), "venue"),
                 string(manifest_.at("contract"), "symbol")};
  for (std::size_t i = 1; i < records.size(); ++i) {
    const auto &command = records[i];
    const auto id = string(command, "request_id");
    if (commands_.contains(id))
      throw std::invalid_argument("交易日志包含重复请求，拒绝恢复");
    apply(*engine_, authorization_, replay_, schedule_, command);
    commands_.emplace(id, command);
  }
}
PaperSession::~PaperSession() {
  if (engine_)
    engine_->stop();
}
void PaperSession::execute(const Json &command) {
  if (failed_)
    throw std::runtime_error("提交状态不确定，请关闭并重新打开会话恢复");
  const auto id = string(command, "request_id");
  if (auto it = commands_.find(id); it != commands_.end()) {
    if (it->second != command)
      throw std::invalid_argument("请求标识被不同操作重复使用");
    return;
  }
  auto candidate = *engine_;
  auto authorization = authorization_;
  auto replay = replay_;
  auto schedule = schedule_;
  apply(candidate, authorization, replay, schedule, command);
  // Allocate all in-memory state before committing durable state.
  auto commands = commands_;
  commands.emplace(id, command);
  try {
    journal_.append(command);
  } catch (...) {
    failed_ = true;
    throw;
  }
  *engine_ = std::move(candidate);
  authorization_.swap(authorization);
  replay_.swap(replay);
  schedule_.swap(schedule);
  commands_.swap(commands);
}
Json PaperSession::snapshot() const {
  auto result = engine_->snapshot();
  result["contract"] = manifest_.at("contract");
  result["costs"] = manifest_.at("costs");
  result["risk"] = manifest_.at("risk");
  result["persistent"] = true;
  result["storage_state"] = failed_ ? "recovery_required" : "ready";
  if (!replay_.is_null()) {
    result["replay"] = replay_;
    const auto cursor = result.at("cursor").get<std::size_t>();
    result["replay"]["session_end"] = cursor && schedule_->event(cursor-1).session_end;
    result["replay"]["settlement_due"] = cursor && schedule_->event(cursor-1).day_end && replay_.at("settled_days") == schedule_->event(cursor-1).day;
  }
  if (!authorization_.is_null())
    result["strategy"] = authorization_;
  return result;
}
} // namespace asterion::trading
