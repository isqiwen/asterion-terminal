#include <asterion/domain/trading_days.hpp>
#include <asterion/domain/futures.hpp>
#include <asterion/domain/historical_bars.hpp>
#include <asterion/domain/history_tables.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <stdexcept>
#include <cmath>
#include <set>
#include <limits>
namespace asterion::protocol {
namespace {
Json closure_evidence(const data::v1::SessionClosureEvidence& item, const std::string& date) {
  if (item.rule_id().empty() || item.version() != 1 || item.exchange().empty() ||
      item.source_url().empty())
    throw std::invalid_argument("invalid historical closure evidence");
  (void)parse_trading_date(item.first_day());
  (void)parse_trading_date(item.last_day());
  (void)parse_trading_date(item.published_day());
  if (date < item.first_day() || date > item.last_day())
    throw std::invalid_argument("invalid historical closure evidence");
  return {{"rule_id", item.rule_id()},      {"version", item.version()},
          {"exchange", item.exchange()},    {"first_day", item.first_day()},
          {"last_day", item.last_day()},    {"published_day", item.published_day()},
          {"source_url", item.source_url()}};
}
} // namespace
void validate_minute_range_query(const data::v1::MinuteRangeQuery& query) {
  validate_message(query);
  if (query.session_rules_version() != 1)
    throw std::invalid_argument("unsupported historical session rules version");
  data::v1::MinuteEvidenceQuery identity;
  identity.set_dataset_id(query.dataset_id());
  identity.set_limit(1);
  validate_minute_evidence_query(identity);
  validate_daily_table(query.daily());
  const auto first = std::chrono::sys_days(parse_trading_date(query.begin_day()));
  const auto last = std::chrono::sys_days(parse_trading_date(query.end_day()));
  if (last < first || (last - first).count() > 365)
    throw std::invalid_argument(
        "minute range audit requires at most 366 calendar days in ascending order");
}
data::v1::MinuteRangeQuery encode_minute_range_query(const Json& value) {
  require_fields(value, {"dataset_id", "daily", "begin_day", "end_day", "session_rules_version"});
  data::v1::MinuteRangeQuery query;
  query.set_dataset_id(value.at("dataset_id").get<std::string>());
  *query.mutable_daily() = encode_daily_table(value.at("daily"));
  query.set_begin_day(value.at("begin_day").get<std::string>());
  query.set_end_day(value.at("end_day").get<std::string>());
  if (!value.at("session_rules_version").is_number_integer() ||
      value.at("session_rules_version") != 1)
    throw std::invalid_argument("unsupported historical session rules version");
  query.set_session_rules_version(1);
  validate_minute_range_query(query);
  return query;
}
Json decode_minute_range_evidence(const data::v1::MinuteRangeEvidence& value) {
  validate_message(value);
  validate_minute_range_query(value.query());
  if (value.request_begin_ns() < 0 || value.request_end_ns() < value.request_begin_ns())
    throw std::invalid_argument("invalid minute range evidence");
  auto day = std::chrono::sys_days(parse_trading_date(value.query().begin_day()));
  const auto last = std::chrono::sys_days(parse_trading_date(value.query().end_day()));
  Json rows = Json::array();
  for (const auto& row : value.days()) {
    if (day > last || row.trading_day() != format_trading_date(std::chrono::year_month_day(day)) ||
        !data::v1::MinuteRangeState_IsValid(row.state()) ||
        row.state() == data::v1::MINUTE_RANGE_UNSPECIFIED)
      throw std::invalid_argument("invalid minute range evidence");
    Json observed = nullptr;
    if (row.has_observed()) {
      const auto& o = row.observed();
      if (o.trading_day() != row.trading_day() || !o.rows() ||
          o.first_ns() < value.request_begin_ns() || o.last_ns() > value.request_end_ns() ||
          o.first_ns() > o.last_ns())
        throw std::invalid_argument("invalid minute range evidence");
      observed = {{"rows", o.rows()},
                  {"first_ns", std::to_string(o.first_ns())},
                  {"last_ns", std::to_string(o.last_ns())}};
    }
    rows.push_back(
        {{"trading_day", row.trading_day()},
         {"state", data::v1::MinuteRangeState_Name(row.state())},
         {"observed", std::move(observed)},
         {"official_closure", row.has_official_closure()
                                  ? closure_evidence(row.official_closure(), row.trading_day())
                                  : Json(nullptr)},
         {"evening_closure", row.has_evening_closure()
                                 ? closure_evidence(row.evening_closure(), row.trading_day())
                                 : Json(nullptr)}});
    day += std::chrono::days(1);
  }
  if (day != last + std::chrono::days(1))
    throw std::invalid_argument("invalid minute range evidence");
  return {{"dataset_id", value.query().dataset_id()},
          {"session_rules_version", value.query().session_rules_version()},
          {"daily", decode_daily_table(value.query().daily())},
          {"begin_day", value.query().begin_day()},
          {"end_day", value.query().end_day()},
          {"contract_id", value.contract_id()},
          {"minute_source", value.minute_source()},
          {"interval_minutes", value.interval_minutes()},
          {"request_begin_ns", std::to_string(value.request_begin_ns())},
          {"request_end_ns", std::to_string(value.request_end_ns())},
          {"unknown_rows", value.unknown_rows()},
          {"request_session_coverage", "unverified"},
          {"days", std::move(rows)}};
}
void validate_minute_evidence_query(const data::v1::MinuteEvidenceQuery& query) {
  validate_message(query);
  if (query.dataset_id().size() != 64 ||
      query.dataset_id().find_first_not_of("0123456789abcdef") != std::string::npos ||
      query.limit() < 1 || query.limit() > 200)
    throw std::invalid_argument("invalid minute evidence query");
}
data::v1::MinuteEvidenceQuery encode_minute_evidence_query(const Json& value) {
  require_fields(value, {"dataset_id", "offset", "limit"});
  for (const auto* field : {"offset", "limit"})
    if (!value.at(field).is_number_integer() || value.at(field) < 0 ||
        value.at(field) > std::numeric_limits<std::uint32_t>::max())
      throw std::invalid_argument("invalid minute evidence query");
  data::v1::MinuteEvidenceQuery query;
  query.set_dataset_id(value.at("dataset_id").get<std::string>());
  query.set_offset(value.at("offset").get<std::uint32_t>());
  query.set_limit(value.at("limit").get<std::uint32_t>());
  validate_minute_evidence_query(query);
  return query;
}
Json decode_minute_evidence(const data::v1::MinuteEvidence& value) {
  validate_message(value);
  data::v1::MinuteEvidenceQuery query;
  query.set_dataset_id(value.dataset_id());
  query.set_limit(value.limit());
  validate_minute_evidence_query(query);
  if (value.begin_ns() < 0 || value.end_ns() < value.begin_ns() ||
      value.unknown_rows() > value.total_rows() || value.offset() > value.total_days() ||
      value.days_size() !=
          static_cast<int>(std::min(value.limit(), value.total_days() - value.offset())))
    throw std::invalid_argument("invalid minute evidence response");
  Json days = Json::array();
  std::string previous;
  std::uint64_t rows = 0;
  for (const auto& day : value.days()) {
    (void)parse_trading_date(day.trading_day());
    if (day.trading_day() <= previous || !day.rows() || day.first_ns() < value.begin_ns() ||
        day.last_ns() > value.end_ns() || day.first_ns() > day.last_ns() ||
        day.rows() > value.total_rows() - value.unknown_rows() - rows)
      throw std::invalid_argument("invalid minute evidence response");
    previous = day.trading_day();
    rows += day.rows();
    days.push_back({{"trading_day", day.trading_day()},
                    {"rows", day.rows()},
                    {"first_ns", std::to_string(day.first_ns())},
                    {"last_ns", std::to_string(day.last_ns())}});
  }
  if (!value.offset() && value.days_size() == static_cast<int>(value.total_days()) &&
      rows != value.total_rows() - value.unknown_rows())
    throw std::invalid_argument("invalid minute evidence response");
  return {{"dataset_id", value.dataset_id()},
          {"begin_ns", std::to_string(value.begin_ns())},
          {"end_ns", std::to_string(value.end_ns())},
          {"total_rows", value.total_rows()},
          {"unknown_rows", value.unknown_rows()},
          {"total_days", value.total_days()},
          {"offset", value.offset()},
          {"limit", value.limit()},
          {"days", std::move(days)}};
}
Json decode_minute_page(const data::v1::MinutePage& page) {
  validate_message(page);
  validate_history_source(page.source());
  (void)HistoryIdentity::parse(page.contract_id());
  if (page.version() != 2 || page.task_id().empty() || page.limit() < 1 || page.limit() > 200 ||
      page.bars_size() > static_cast<int>(page.limit()) ||
      page.matched_rows() > page.total_rows() || page.total_rows() > 60000000 ||
      page.offset() > page.matched_rows() ||
      page.offset() + page.bars_size() > page.matched_rows() ||
      static_cast<std::uint64_t>(page.bars_size()) !=
          std::min<std::uint64_t>(page.limit(), page.matched_rows() - page.offset()) ||
      page.begin_ns() > page.end_ns() || page.manifest_sha256().size() != 64 ||
      page.source().empty() ||
      (page.total_rows() && (page.first_ns() <= 0 || page.first_ns() > page.last_ns())))
    throw std::invalid_argument("invalid minute dataset page response");
  Json bars = Json::array();
  std::int64_t previous = -1;
  for (const auto& row : page.bars()) {
    if (!row.has_open() || !row.has_high() || !row.has_low() || !row.has_close() ||
        !row.has_volume() || !row.has_amount() || !row.has_open_interest())
      throw std::invalid_argument("invalid minute dataset page response");
    HistoricalBar bar{row.timestamp_ns(),
                      Decimal::from_raw(row.open().units()),
                      Decimal::from_raw(row.high().units()),
                      Decimal::from_raw(row.low().units()),
                      Decimal::from_raw(row.close().units()),
                      Decimal::from_raw(row.volume().units()),
                      Decimal::from_raw(row.amount().units()),
                      Decimal::from_raw(row.open_interest().units())};
    bar.validate();
    if (bar.timestamp_ns <= previous || bar.timestamp_ns < page.begin_ns() ||
        bar.timestamp_ns > page.end_ns())
      throw std::invalid_argument("invalid minute dataset page response");
    previous = bar.timestamp_ns;
    bars.push_back({{"timestamp_ns", std::to_string(bar.timestamp_ns)},
                    {"open", bar.open.str()},
                    {"high", bar.high.str()},
                    {"low", bar.low.str()},
                    {"close", bar.close.str()},
                    {"volume", bar.volume.str()},
                    {"amount", bar.amount.str()},
                    {"open_interest", bar.open_interest.str()}});
    if (row.has_macd()) {
      const auto& macd = row.macd();
      if (!std::isfinite(macd.diff()) || !std::isfinite(macd.signal()) ||
          !std::isfinite(macd.histogram()))
        throw std::invalid_argument("invalid minute dataset page response");
      bars.back()["macd"] = {
          {"diff", macd.diff()}, {"signal", macd.signal()}, {"histogram", macd.histogram()}};
    }
  }
  return {{"id", page.task_id()},
          {"source", page.source()},
          {"contract_id", page.contract_id()},
          {"interval_minutes", page.interval_minutes()},
          {"manifest_sha256", page.manifest_sha256()},
          {"total_rows", page.total_rows()},
          {"matched_rows", page.matched_rows()},
          {"offset", page.offset()},
          {"limit", page.limit()},
          {"first_ns", std::to_string(page.first_ns())},
          {"last_ns", std::to_string(page.last_ns())},
          {"begin_ns", std::to_string(page.begin_ns())},
          {"end_ns", std::to_string(page.end_ns())},
          {"bars", bars}};
}
data::v1::HistoryUpdateQuery encode_history_update_query(const Json& value) {
  require_fields(value, {"dataset_id", "daily", "mode", "end_day", "requests_per_minute"});
  data::v1::HistoryUpdateQuery result;
  result.set_dataset_id(value.at("dataset_id").get<std::string>());
  if (!value.at("daily").is_null())
    *result.mutable_daily() = encode_daily_table(value.at("daily"));
  result.set_end_day(value.at("end_day").get<std::string>());
  const auto mode = value.at("mode").get<std::string>();
  if (mode != "extend" && mode != "repair")
    throw std::invalid_argument("invalid history update request");
  result.set_mode(mode == "extend" ? data::v1::EXTEND : data::v1::REPAIR);
  if (!value.at("requests_per_minute").is_number_integer() || value.at("requests_per_minute") < 1 ||
      value.at("requests_per_minute") > 500)
    throw std::invalid_argument("invalid history update request");
  result.set_requests_per_minute(value.at("requests_per_minute").get<unsigned>());
  (void)decode_history_update_query(result);
  return result;
}
Json decode_history_update_query(const data::v1::HistoryUpdateQuery& value) {
  validate_message(value);
  const auto digest = [](const std::string& id) {
    return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos;
  };
  if (!digest(value.dataset_id()) || value.requests_per_minute() < 1 ||
      value.requests_per_minute() > 500 ||
      (value.mode() != data::v1::EXTEND && value.mode() != data::v1::REPAIR))
    throw std::invalid_argument("invalid history update request");
  if (value.mode() == data::v1::EXTEND) {
    if (value.has_daily())
      throw std::invalid_argument("invalid history update request");
    (void)parse_trading_date(value.end_day());
  } else if (!value.has_daily() || !value.end_day().empty())
    throw std::invalid_argument("invalid history update request");
  return {{"dataset_id", value.dataset_id()},
          {"daily", value.has_daily() ? decode_daily_table(value.daily()) : Json(nullptr)},
          {"mode", value.mode() == data::v1::EXTEND ? "extend" : "repair"},
          {"end_day", value.end_day()},
          {"requests_per_minute", value.requests_per_minute()}};
}
Json decode_history_update_plan(const data::v1::HistoryUpdatePlan& value) {
  validate_message(value);
  auto canonical = value;
  canonical.clear_id();
  if (value.id() != sha256_bytes(canonical.SerializeAsString()) || !value.has_minutes())
    throw std::invalid_argument("history update plan changed; preview again");
  const auto query = decode_history_update_query(value.query());
  std::vector<std::string> days;
  for (const auto& day : value.missing_days()) {
    (void)parse_trading_date(day);
    if (!days.empty() && days.back() >= day)
      throw std::invalid_argument("invalid history update request");
    days.push_back(day);
  }
  Json conflicts = Json::array();
  if ((value.query().mode() == data::v1::REPAIR && value.session_rules_version() != 1) ||
      (value.query().mode() == data::v1::EXTEND &&
       (value.session_rules_version() || !value.conflicts().empty())))
    throw std::invalid_argument("invalid history repair conflict evidence");
  std::string previous_conflict;
  for (const auto& conflict : value.conflicts()) {
    (void)parse_trading_date(conflict.day());
    if (conflict.day() <= previous_conflict || !conflict.has_closure() ||
        (!conflict.observed_minutes() && !conflict.calendar_open() && !conflict.daily_auction()) ||
        !days.empty())
      throw std::invalid_argument("invalid history repair conflict evidence");
    previous_conflict = conflict.day();
    if (conflict.closure().exchange() !=
        HistoryIdentity::parse(value.minutes().contract_id()).venue)
      throw std::invalid_argument("invalid history repair conflict evidence");
    conflicts.push_back({{"day", conflict.day()},
                         {"closure", closure_evidence(conflict.closure(), conflict.day())},
                         {"observed_minutes", conflict.observed_minutes()},
                         {"calendar_open", conflict.calendar_open()},
                         {"daily_auction", conflict.daily_auction()}});
  }
  const auto& minutes = value.minutes();
  validate_history_source(minutes.source());
  (void)HistoryIdentity::parse(minutes.contract_id());
  return {{"id", value.id()},
          {"query", query},
          {"source", minutes.source()},
          {"contract_id", minutes.contract_id()},
          {"interval_minutes", minutes.interval_minutes()},
          {"begin", format_shanghai_time(minutes.begin_ns())},
          {"end", format_shanghai_time(minutes.end_ns())},
          {"missing_days", days},
          {"conflicts", std::move(conflicts)},
          {"session_rules_version", value.session_rules_version()}};
}
Json decode_table_refresh_query(const data::v1::TableRefreshQuery& query) {
  validate_message(query);
  validate_id(query.provider());
  validate_history_source(query.source());
  const auto& key = query.partition();
  if (history_table(key.table()).period != TablePeriod::day ||
      query.previous_snapshot().size() != 64 ||
      query.previous_snapshot().find_first_not_of("0123456789abcdef") != std::string::npos ||
      query.requests_per_minute() < 1 || query.requests_per_minute() > 500)
    throw std::invalid_argument("invalid table refresh selection");
  validate_history_venue(key.exchange());
  (void)parse_trading_date(key.period());
  return {{"provider", query.provider()},
          {"source", query.source()},
          {"table", key.table()},
          {"exchange", key.exchange()},
          {"period", key.period()},
          {"previous_snapshot", query.previous_snapshot()},
          {"requests_per_minute", query.requests_per_minute()}};
}
data::v1::TableRefreshQuery encode_table_refresh_query(const Json& value) {
  require_fields(value, {"provider", "source", "table", "exchange", "period", "previous_snapshot",
                         "requests_per_minute"});
  if (!value.at("requests_per_minute").is_number_integer() || value.at("requests_per_minute") < 1 ||
      value.at("requests_per_minute") > 500)
    throw std::invalid_argument("invalid table refresh selection");
  data::v1::TableRefreshQuery query;
  query.set_provider(value.at("provider").get<std::string>());
  query.set_source(value.at("source").get<std::string>());
  query.set_previous_snapshot(value.at("previous_snapshot").get<std::string>());
  query.set_requests_per_minute(value.at("requests_per_minute").get<unsigned>());
  auto* key = query.mutable_partition();
  key->set_table(value.at("table").get<std::string>());
  key->set_exchange(value.at("exchange").get<std::string>());
  key->set_period(value.at("period").get<std::string>());
  (void)decode_table_refresh_query(query);
  return query;
}
void validate_table_update(const data::v1::TableUpdate& update) {
  validate_message(update);
  validate_id(update.provider());
  if (update.version() != 1 || update.requests_per_minute() < 1 ||
      update.requests_per_minute() > 500 || update.tables().empty() || update.tables_size() > 32)
    throw std::invalid_argument("invalid table update definition");
  std::string_view previous;
  for (const auto& table : update.tables()) {
    if (history_table(table).period != TablePeriod::day || table <= previous)
      throw std::invalid_argument("invalid table update definition");
    previous = table;
  }
  for (const auto* day : {&update.begin_day(), &update.end_day()})
    if (!day->empty())
      (void)parse_trading_date(*day);
  if (!update.begin_day().empty() && !update.end_day().empty() &&
      update.begin_day() > update.end_day())
    throw std::invalid_argument("invalid table update definition");
  if (update.has_refresh()) {
    const auto& refresh = update.refresh();
    data::v1::TableRefreshQuery query;
    query.set_provider(update.provider());
    query.set_source(refresh.source());
    *query.mutable_partition() = refresh.partition();
    query.set_previous_snapshot(refresh.previous_snapshot());
    query.set_requests_per_minute(update.requests_per_minute());
    (void)decode_table_refresh_query(query);
    const auto digest = [](const std::string& text) {
      return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
    };
    if (update.tables_size() != 1 || update.tables(0) != refresh.partition().table() ||
        update.begin_day() != refresh.partition().period() ||
        update.end_day() != update.begin_day() || !digest(refresh.previous_content()) ||
        !digest(refresh.provider_artifact()))
      throw std::invalid_argument("invalid table refresh selection");
  }
}
data::v1::TableUpdate encode_table_update(const Json& value) {
  if (value.contains("refresh"))
    require_fields(
        value, {"provider", "tables", "begin_day", "end_day", "requests_per_minute", "refresh"});
  else
    require_fields(value, {"provider", "tables", "begin_day", "end_day", "requests_per_minute"});
  const auto& rate = value.at("requests_per_minute");
  if (!value.at("tables").is_array() || !rate.is_number_integer() || rate < 1 || rate > 500)
    throw std::invalid_argument("invalid table update definition");
  data::v1::TableUpdate update;
  update.set_version(1);
  update.set_provider(value.at("provider").get<std::string>());
  for (const auto& table : value.at("tables"))
    update.add_tables(table.get<std::string>());
  update.set_begin_day(value.at("begin_day").get<std::string>());
  update.set_end_day(value.at("end_day").get<std::string>());
  update.set_requests_per_minute(value.at("requests_per_minute").get<unsigned>());
  if (value.contains("refresh")) {
    const auto& ref = value.at("refresh");
    require_fields(
        ref, {"partition", "source", "previous_snapshot", "previous_content", "provider_artifact"});
    const auto& key = ref.at("partition");
    require_fields(key, {"table", "exchange", "period"});
    auto* target = update.mutable_refresh();
    target->mutable_partition()->set_table(key.at("table").get<std::string>());
    target->mutable_partition()->set_exchange(key.at("exchange").get<std::string>());
    target->mutable_partition()->set_period(key.at("period").get<std::string>());
    target->set_source(ref.at("source").get<std::string>());
    target->set_previous_snapshot(ref.at("previous_snapshot").get<std::string>());
    target->set_previous_content(ref.at("previous_content").get<std::string>());
    target->set_provider_artifact(ref.at("provider_artifact").get<std::string>());
  }
  validate_table_update(update);
  return update;
}
Json decode_table_update(const data::v1::TableUpdate& update) {
  validate_table_update(update);
  Json result = {
      {"provider", update.provider()},
      {"tables", std::vector<std::string>(update.tables().begin(), update.tables().end())},
      {"begin_day", update.begin_day()},
      {"end_day", update.end_day()},
      {"requests_per_minute", update.requests_per_minute()}};
  if (update.has_refresh()) {
    const auto& ref = update.refresh();
    result["refresh"] = {{"partition",
                          {{"table", ref.partition().table()},
                           {"exchange", ref.partition().exchange()},
                           {"period", ref.partition().period()}}},
                         {"source", ref.source()},
                         {"previous_snapshot", ref.previous_snapshot()},
                         {"previous_content", ref.previous_content()},
                         {"provider_artifact", ref.provider_artifact()}};
  }
  return result;
}
namespace {
Json partition_key(const data::v1::TablePartitionKey& key) {
  return {{"table", key.table()}, {"exchange", key.exchange()}, {"period", key.period()}};
}
Json table_revision(const data::v1::TableRevision& revision) {
  for (const auto* digest : {&revision.content(), &revision.object(), &revision.snapshot()})
    if (digest->size() != 64 || digest->find_first_not_of("0123456789abcdef") != std::string::npos)
      throw std::invalid_argument("invalid history table revision");
  if (!revision.sequence() || !revision.rows())
    throw std::invalid_argument("invalid history table revision");
  return {{"sequence", revision.sequence()},
          {"table", revision.table()},
          {"schema", revision.schema()},
          {"source", revision.source()},
          {"exchange", revision.exchange()},
          {"period", revision.period()},
          {"content", revision.content()},
          {"rows", revision.rows()},
          {"bytes", revision.bytes()},
          {"acquired_at_ns", std::to_string(revision.acquired_at_ns())},
          {"provider_artifact", revision.provider_artifact()},
          {"snapshot", revision.snapshot()}};
}
Json refresh_observation(const data::v1::TableRefreshObservation& o) {
  if (o.id().size() != 64 || o.operation().size() != 64 || !o.attempt() || !o.has_revision() ||
      !o.has_response() || o.response().format() != "tushare.fut_daily.raw.v1" ||
      o.response().received_at_ns() <= 0 || o.recorded_at_ns() <= 0 ||
      o.response().body().empty() || o.response().body().size() > 1024 * 1024 ||
      o.response_bytes() != o.response().body().size() ||
      o.response_sha256() != sha256_bytes(o.response().body()))
    throw std::invalid_argument("invalid table refresh observation");
  return {{"kind", "explicit_refresh_response_observation"},
          {"version", 1},
          {"id", o.id()},
          {"operation", o.operation()},
          {"data_instance", o.data_instance()},
          {"task_instance", o.task_instance()},
          {"task_id", o.task_id()},
          {"attempt", o.attempt()},
          {"provider", o.provider()},
          {"provider_artifact", o.provider_artifact()},
          {"selected_snapshot", o.selected_snapshot()},
          {"selected_content", o.selected_content()},
          {"revision", table_revision(o.revision())},
          {"normalized_object", o.revision().object()},
          {"format", o.response().format()},
          {"body", o.response().body()},
          {"response_sha256", o.response_sha256()},
          {"response_bytes", o.response_bytes()},
          {"received_at_ns", std::to_string(o.response().received_at_ns())},
          {"recorded_at_ns", std::to_string(o.recorded_at_ns())},
          {"unchanged", o.unchanged()},
          {"content_changed_from_selected", o.selected_content() != o.revision().content()}};
}
} // namespace
Json decode_table_refresh_plan(const data::v1::TableRefreshPlan& value) {
  validate_message(value);
  auto canonical = value;
  canonical.clear_id();
  if (value.id() != sha256_bytes(canonical.SerializeAsString()) || !value.has_previous() ||
      !value.update().has_refresh())
    throw std::invalid_argument("table refresh plan changed; preview again");
  const auto query = decode_table_refresh_query(value.query());
  const auto update = decode_table_update(value.update());
  const auto& query_key = value.query().partition();
  const auto& target = value.update().refresh();
  const auto& previous = value.previous();
  if (value.update().provider() != value.query().provider() ||
      value.update().requests_per_minute() != value.query().requests_per_minute() ||
      target.partition().SerializeAsString() != query_key.SerializeAsString() ||
      target.source() != value.query().source() ||
      target.previous_snapshot() != value.query().previous_snapshot() ||
      previous.snapshot() != value.query().previous_snapshot() ||
      previous.content() != target.previous_content() || previous.table() != query_key.table() ||
      previous.exchange() != query_key.exchange() || previous.period() != query_key.period() ||
      previous.source() != value.query().source())
    throw std::invalid_argument("table refresh plan changed; preview again");
  return {{"id", value.id()},
          {"query", query},
          {"previous", table_revision(value.previous())},
          {"update", update}};
}
Json decode_table_update_result(const data::v1::TableUpdateResult& result) {
  validate_message(result);
  if (result.version() != 1 ||
      static_cast<unsigned>(result.unpublished_examples_size()) > result.unpublished())
    throw std::invalid_argument("invalid table update result");
  Json rejected = Json::array(), unpublished = Json::array();
  for (const auto& item : result.rejected()) {
    auto row = partition_key(item.partition());
    row["reason"] = item.reason();
    rejected.push_back(std::move(row));
  }
  for (const auto& item : result.unpublished_examples())
    unpublished.push_back(partition_key(item));
  Json result_json = {
      {"stored", result.stored()},
      {"unchanged", result.unchanged()},
      {"rejected", std::move(rejected)},
      {"unpublished", result.unpublished()},
      {"unpublished_examples", std::move(unpublished)},
      {"waiting", std::vector<std::string>(result.waiting().begin(), result.waiting().end())},
      {"snapshot", result.snapshot()}};
  if (result.has_refreshed_revision())
    result_json["refreshed_revision"] = table_revision(result.refreshed_revision());
  if (result.has_refresh_observation())
    result_json["refresh_observation"] = refresh_observation(result.refresh_observation());
  return result_json;
}
Json decode_table_coverage(const data::v1::TableCoverage& coverage) {
  validate_message(coverage);
  Json sources = Json::array(), rows = Json::array();
  for (const auto& item : coverage.sources())
    sources.push_back(
        {{"table", item.table()},
         {"schema", item.schema()},
         {"source", item.source()},
         {"plugin_id", item.plugin_id()},
         {"venues", std::vector<std::string>(item.venues().begin(), item.venues().end())}});
  for (const auto& item : coverage.rows())
    rows.push_back({{"table", item.table()},
                    {"schema", item.schema()},
                    {"source", item.source()},
                    {"exchange", item.exchange()},
                    {"partitions", item.partitions()},
                    {"rows", item.rows()},
                    {"first_period", item.first_period()},
                    {"last_period", item.last_period()},
                    {"acquired_at_ns", std::to_string(item.acquired_at_ns())},
                    {"missing_days", item.has_missing_days() ? Json(item.missing_days()) : Json()},
                    {"missing_examples", std::vector<std::string>(item.missing_examples().begin(),
                                                                  item.missing_examples().end())}});
  return {{"snapshot", coverage.snapshot()},
          {"sources", std::move(sources)},
          {"rows", std::move(rows)}};
}
data::v1::TablePageQuery encode_table_page_query(const Json& value) {
  require_fields(value, {"table", "source", "exchange", "period", "offset", "limit"});
  if (!value.at("offset").is_number_integer() || value.at("offset") < 0 ||
      !value.at("limit").is_number_integer() || value.at("limit") < 1 || value.at("limit") > 500)
    throw std::invalid_argument("invalid history table page query");
  data::v1::TablePageQuery query;
  query.mutable_partition()->set_table(value.at("table").get<std::string>());
  query.mutable_partition()->set_exchange(value.at("exchange").get<std::string>());
  query.mutable_partition()->set_period(value.at("period").get<std::string>());
  query.set_source(value.at("source").get<std::string>());
  query.set_offset(value.at("offset").get<std::uint64_t>());
  query.set_limit(value.at("limit").get<unsigned>());
  validate_table_period(history_table(query.partition().table()), query.partition().period());
  validate_history_venue(query.partition().exchange());
  validate_history_source(query.source());
  return query;
}
Json decode_table_page(const data::v1::TablePage& page) {
  validate_message(page);
  Json rows = Json::array();
  for (const auto& row : page.rows()) {
    if (row.cells_size() != page.columns_size())
      throw std::invalid_argument("invalid history table page");
    Json cells = Json::array();
    for (const auto& cell : row.cells())
      cells.push_back(cell.has_text() ? Json(cell.text()) : Json());
    rows.push_back(std::move(cells));
  }
  Json evidence = nullptr;
  if (page.has_response_evidence()) {
    const auto& stored = page.response_evidence();
    const auto& raw = stored.response();
    if (!stored.has_response() || raw.format() != "tushare.fut_daily.raw.v1" ||
        raw.received_at_ns() <= 0 || raw.body().empty() || raw.body().size() > 1024 * 1024 ||
        stored.bytes() != raw.body().size() ||
        stored.revision_snapshot() != page.revision().snapshot() ||
        stored.normalized_content() != page.revision().content() ||
        stored.normalized_object() != page.revision().object() ||
        stored.provider_artifact() != page.revision().provider_artifact())
      throw std::invalid_argument("invalid source response evidence");
    evidence = {{"kind", "raw_response_body"},
                {"format", raw.format()},
                {"received_at_ns", std::to_string(raw.received_at_ns())},
                {"sha256", stored.sha256()},
                {"bytes", stored.bytes()},
                {"revision_snapshot", stored.revision_snapshot()},
                {"normalized_content", stored.normalized_content()},
                {"normalized_object", stored.normalized_object()},
                {"provider_artifact", stored.provider_artifact()},
                {"body", raw.body()}};
  }
  Json review = nullptr;
  if (page.has_response_review()) {
    const auto& checked = page.response_review();
    Json differences = Json::array();
    for (const auto& issue : checked.differences())
      differences.push_back({{"kind", issue.kind()},
                             {"key", issue.key()},
                             {"field", issue.field()},
                             {"raw", issue.has_raw() ? Json(issue.raw()) : Json()},
                             {"expected", issue.has_expected() ? Json(issue.expected()) : Json()},
                             {"actual", issue.has_actual() ? Json(issue.actual()) : Json()}});
    review = {{"status", checked.status()},
              {"reason", checked.reason()},
              {"response_fields", checked.response_fields()},
              {"response_rows", checked.response_rows()},
              {"normalized_rows", checked.normalized_rows()},
              {"dated_rows", checked.dated_rows()},
              {"matched_rows", checked.matched_rows()},
              {"compared_cells", checked.compared_cells()},
              {"skipped_keys", std::vector<std::string>(checked.skipped_keys().begin(),
                                                        checked.skipped_keys().end())},
              {"unknown_keys", std::vector<std::string>(checked.unknown_keys().begin(),
                                                        checked.unknown_keys().end())},
              {"differences", std::move(differences)}};
  }
  return {
      {"latest_refresh_observation", page.has_latest_refresh_observation()
                                         ? refresh_observation(page.latest_refresh_observation())
                                         : Json()},
      {"response_review", std::move(review)},
      {"response_evidence", std::move(evidence)},
      {"revision", table_revision(page.revision())},
      {"columns", std::vector<std::string>(page.columns().begin(), page.columns().end())},
      {"rows", std::move(rows)},
      {"offset", page.offset()}};
}
Json decode_history_usage(const data::v1::HistoryUsage& value) {
  validate_message(value);
  if (value.dataset_id().size() != 64 ||
      value.dataset_id().find_first_not_of("0123456789abcdef") != std::string::npos ||
      value.references_size() > 10000)
    throw std::invalid_argument("invalid historical usage response");
  Json rows = Json::array();
  std::set<std::pair<int, std::string>> identities;
  for (const auto& row : value.references()) {
    std::string kind;
    switch (row.kind()) {
    case data::v1::HISTORY_DOWNLOAD:
      kind = "download";
      break;
    case data::v1::HISTORY_BACKTEST:
      kind = "backtest";
      break;
    case data::v1::HISTORY_FACTOR:
      kind = "factor";
      break;
    case data::v1::HISTORY_SAVED_DATASET:
      kind = "saved_dataset";
      break;
    default:
      throw std::invalid_argument("invalid historical usage response");
    }
    if (row.id().empty() || row.roles().empty() || !identities.emplace(row.kind(), row.id()).second)
      throw std::invalid_argument("invalid historical usage response");
    Json roles = Json::array();
    int previous = 0;
    for (const auto role : row.roles()) {
      if (role <= previous ||
          (role != data::v1::HISTORY_MARKET && role != data::v1::HISTORY_OUTPUT) ||
          ((row.kind() == data::v1::HISTORY_DOWNLOAD) != (role == data::v1::HISTORY_OUTPUT)))
        throw std::invalid_argument("invalid historical usage response");
      previous = role;
      roles.push_back(role == data::v1::HISTORY_MARKET ? "market" : "output");
    }
    rows.push_back({{"kind", kind}, {"id", row.id()}, {"name", row.name()}, {"roles", roles}});
  }
  return {
      {"dataset_id", value.dataset_id()}, {"references", rows}, {"unreadable", value.unreadable()}};
}
} // namespace asterion::protocol
