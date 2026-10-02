#include <asterion/domain/daily_bars.hpp>
#include <asterion/domain/futures.hpp>
#include <asterion/domain/historical_bars.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <stdexcept>
#include <cmath>
#include <set>
namespace asterion::protocol {
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
data::v1::DailyBar encode_daily_bar(const HistoricalDailyBar& bar) {
  bar.validate();
  data::v1::DailyBar row;
  row.set_trading_day(format_trading_date(bar.trading_day));
  row.mutable_open()->set_units(bar.open.raw());
  row.mutable_high()->set_units(bar.high.raw());
  row.mutable_low()->set_units(bar.low.raw());
  row.mutable_close()->set_units(bar.close.raw());
  row.mutable_volume()->set_units(bar.volume.raw());
  row.mutable_amount()->set_units(bar.amount.raw());
  row.mutable_open_interest()->set_units(bar.open_interest.raw());
  if (bar.previous_close)
    row.mutable_previous_close()->set_units(bar.previous_close->raw());
  if (bar.previous_settlement)
    row.mutable_previous_settlement()->set_units(bar.previous_settlement->raw());
  if (bar.settlement)
    row.mutable_settlement()->set_units(bar.settlement->raw());
  return row;
}
HistoricalDailyBar daily_bar(const data::v1::DailyBar& row) {
  validate_message(row);
  if (!row.has_open() || !row.has_high() || !row.has_low() || !row.has_close() ||
      !row.has_volume() || !row.has_amount() || !row.has_open_interest())
    throw std::invalid_argument("invalid daily dataset page response");
  HistoricalDailyBar bar{parse_trading_date(row.trading_day()),
                         Decimal::from_raw(row.open().units()),
                         Decimal::from_raw(row.high().units()),
                         Decimal::from_raw(row.low().units()),
                         Decimal::from_raw(row.close().units()),
                         Decimal::from_raw(row.volume().units()),
                         Decimal::from_raw(row.amount().units()),
                         Decimal::from_raw(row.open_interest().units()),
                         {},
                         {},
                         {}};
  if (row.has_previous_close())
    bar.previous_close = Decimal::from_raw(row.previous_close().units());
  if (row.has_previous_settlement())
    bar.previous_settlement = Decimal::from_raw(row.previous_settlement().units());
  if (row.has_settlement())
    bar.settlement = Decimal::from_raw(row.settlement().units());
  bar.validate();
  return bar;
}
Json decode_daily_page(const data::v1::DailyPage& page) {
  validate_message(page);
  validate_history_source(page.source());
  (void)HistoryIdentity::parse(page.contract_id());
  validate_id(page.task_id());
  const auto begin = parse_trading_date(page.begin_day());
  const auto end = parse_trading_date(page.end_day());
  if (page.version() != 2 || page.limit() < 1 || page.limit() > 200 || begin > end ||
      !data::v1::DailyPeriod_IsValid(page.period()) || page.source().empty() ||
      page.contract_id().empty() || page.total_rows() > 20 * 366 + 1 ||
      page.matched_rows() > page.total_rows() || page.offset() > page.matched_rows() ||
      (page.matched_rows() && page.offset() == page.matched_rows()) ||
      static_cast<std::uint64_t>(page.bars_size()) !=
          std::min<std::uint64_t>(page.limit(), page.matched_rows() - page.offset()) ||
      page.manifest_sha256().size() != 64 ||
      page.manifest_sha256().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid daily dataset page response");
  std::optional<std::chrono::year_month_day> first, last;
  if (page.total_rows()) {
    first = parse_trading_date(page.first_day());
    last = parse_trading_date(page.last_day());
    if (*first > *last ||
        page.total_rows() >
            static_cast<std::uint64_t>(
                (std::chrono::sys_days(*last) - std::chrono::sys_days(*first)).count() + 1))
      throw std::invalid_argument("invalid daily dataset page response");
  } else if (!page.first_day().empty() || !page.last_day().empty())
    throw std::invalid_argument("invalid daily dataset page response");
  Json bars = Json::array();
  std::optional<std::chrono::year_month_day> previous;
  for (const auto& row : page.bars()) {
    const auto bar = daily_bar(row);
    if ((previous && bar.trading_day <= *previous) || bar.trading_day < begin ||
        bar.trading_day > end || !first || bar.trading_day < *first || bar.trading_day > *last ||
        bar.volume.raw() % 100000000 || bar.open_interest.raw() % 100000000)
      throw std::invalid_argument("invalid daily dataset page response");
    previous = bar.trading_day;
    const auto optional = [](const std::optional<Decimal>& value) -> Json {
      return value ? Json(value->str()) : Json(nullptr);
    };
    bars.push_back({{"trading_day", row.trading_day()},
                    {"open", bar.open.str()},
                    {"high", bar.high.str()},
                    {"low", bar.low.str()},
                    {"close", bar.close.str()},
                    {"volume", bar.volume.str()},
                    {"amount", bar.amount.str()},
                    {"open_interest", bar.open_interest.str()},
                    {"previous_close", optional(bar.previous_close)},
                    {"previous_settlement", optional(bar.previous_settlement)},
                    {"settlement", optional(bar.settlement)}});
    if (row.has_macd()) {
      const auto& macd = row.macd();
      if (!std::isfinite(macd.diff()) || !std::isfinite(macd.signal()) ||
          !std::isfinite(macd.histogram()))
        throw std::invalid_argument("invalid daily dataset page response");
      bars.back()["macd"] = {
          {"diff", macd.diff()}, {"signal", macd.signal()}, {"histogram", macd.histogram()}};
    }
  }
  return {{"id", page.task_id()},
          {"period", page.period() == data::v1::DAY       ? "day"
                     : page.period() == data::v1::WEEK    ? "week"
                     : page.period() == data::v1::MONTH   ? "month"
                     : page.period() == data::v1::QUARTER ? "quarter"
                                                          : "year"},
          {"source", page.source()},
          {"contract_id", page.contract_id()},
          {"manifest_sha256", page.manifest_sha256()},
          {"total_rows", page.total_rows()},
          {"matched_rows", page.matched_rows()},
          {"offset", page.offset()},
          {"limit", page.limit()},
          {"first_day", page.first_day()},
          {"last_day", page.last_day()},
          {"begin_day", page.begin_day()},
          {"end_day", page.end_day()},
          {"bars", bars}};
}
data::v1::HistoryUpdateQuery encode_history_update_query(const Json& value) {
  require_fields(value,
                 {"dataset_id", "calendar_dataset_id", "mode", "end_day", "requests_per_minute"});
  data::v1::HistoryUpdateQuery result;
  result.set_dataset_id(value.at("dataset_id").get<std::string>());
  result.set_calendar_dataset_id(value.at("calendar_dataset_id").get<std::string>());
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
    if (!value.calendar_dataset_id().empty())
      throw std::invalid_argument("invalid history update request");
    (void)parse_trading_date(value.end_day());
  } else if (!digest(value.calendar_dataset_id()) || !value.end_day().empty())
    throw std::invalid_argument("invalid history update request");
  return {{"dataset_id", value.dataset_id()},
          {"calendar_dataset_id", value.calendar_dataset_id()},
          {"mode", value.mode() == data::v1::EXTEND ? "extend" : "repair"},
          {"end_day", value.end_day()},
          {"requests_per_minute", value.requests_per_minute()}};
}
Json decode_history_update_plan(const data::v1::HistoryUpdatePlan& value) {
  validate_message(value);
  auto canonical = value;
  canonical.clear_id();
  if (value.id() != sha256_bytes(canonical.SerializeAsString()) ||
      (!value.has_minutes() && !value.has_daily()))
    throw std::invalid_argument("history update plan changed; preview again");
  const auto query = decode_history_update_query(value.query());
  std::vector<std::string> days;
  for (const auto& day : value.missing_days()) {
    (void)parse_trading_date(day);
    if (!days.empty() && days.back() >= day)
      throw std::invalid_argument("invalid history update request");
    days.push_back(day);
  }
  const auto source = value.has_minutes() ? value.minutes().source() : value.daily().source();
  const auto contract =
      value.has_minutes() ? value.minutes().contract_id() : value.daily().contract_id();
  validate_history_source(source);
  (void)HistoryIdentity::parse(contract);
  return {{"id", value.id()},
          {"query", query},
          {"source", source},
          {"contract_id", contract},
          {"interval_minutes", value.has_minutes() ? value.minutes().interval_minutes() : 0},
          {"begin", value.has_minutes() ? format_shanghai_time(value.minutes().begin_ns())
                                        : value.daily().begin_day()},
          {"end", value.has_minutes() ? format_shanghai_time(value.minutes().end_ns())
                                      : value.daily().end_day()},
          {"missing_days", days}};
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
    case data::v1::HISTORY_BAR_FACTOR:
      kind = "bar_factor";
      break;
    case data::v1::HISTORY_DAILY_FACTOR:
      kind = "daily_factor";
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
      if (role <= previous || role > data::v1::HISTORY_OUTPUT ||
          ((row.kind() == data::v1::HISTORY_DOWNLOAD) != (role == data::v1::HISTORY_OUTPUT)))
        throw std::invalid_argument("invalid historical usage response");
      previous = role;
      roles.push_back(role == data::v1::HISTORY_MARKET       ? "market"
                      : role == data::v1::HISTORY_SETTLEMENT ? "settlement"
                                                             : "output");
    }
    rows.push_back({{"kind", kind}, {"id", row.id()}, {"name", row.name()}, {"roles", roles}});
  }
  return {{"dataset_id", value.dataset_id()}, {"references", rows}};
}
} // namespace asterion::protocol
