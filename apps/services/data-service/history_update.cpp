#include "history_update.hpp"
#include "history_coverage.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <chrono>
#include <set>
namespace asterion::data {
namespace {
std::filesystem::path directory(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace
data::v1::HistoryUpdatePlan history_update_plan(const history_files::Archive& archive,
                                                const history_files::TableStore& tables,
                                                const data::v1::HistoryUpdateQuery& query,
                                                std::int64_t now_ns) {
  (void)protocol::decode_history_update_query(query);
  if (query.mode() == data::v1::EXTEND &&
      query.end_day() >= format_shanghai_time(now_ns).substr(0, 10))
    throw std::invalid_argument("incremental downloads require a completed calendar day");
  const auto record = archive.get(query.dataset_id());
  data::v1::HistoryUpdatePlan plan;
  *plan.mutable_query() = query;
  if (record.has_minutes() && record.has_minute_result()) {
    history_files::verify_minute_result(record.minutes(), record.minute_result());
    if (record.minute_result().manifest_sha256() != query.dataset_id())
      throw std::invalid_argument("dataset archive changed during resolution");
    auto input = record.minutes();
    if (query.mode() == data::v1::EXTEND) {
      // The existing download and source timestamp contract use whole seconds.
      // Re-request a fractional boundary second instead of skipping any labels.
      const auto begin = input.end_ns() % 1000000000 ? input.end_ns() / 1000000000 * 1000000000
                                                     : input.end_ns() + 1000000000;
      const auto end = parse_shanghai_time(query.end_day() + " 23:59:59");
      if (begin > end)
        throw std::invalid_argument("no later download range is requested");
      input.set_begin_ns(begin);
      input.set_end_ns(end);
    } else {
      const auto minute_days = history_files::minute_trading_days(
          directory(record.minute_result().directory()), query.dataset_id());
      if (minute_days.empty())
        throw std::invalid_argument("repair requires known minute trading days");
      for (const auto& day :
           minute_coverage(tables, query.daily(),
                           history_files::minute_range(input).instrument.key(),
                           std::set<std::string>(minute_days.begin(), minute_days.end()))
               .uncovered)
        plan.add_missing_days(day);
      if (plan.missing_days().empty())
        throw std::invalid_argument("no missing whole trading days in these versions");
      // Use the exact original window, including all its nights and partial
      // boundaries. A provider may still return missing days; never promise fill.
    }
    input.set_requests_per_minute(query.requests_per_minute());
    (void)history_files::minute_range(input);
    *plan.mutable_minutes() = input;
  } else if (record.has_daily() && record.has_daily_result()) {
    if (query.mode() != data::v1::EXTEND)
      throw std::invalid_argument("repair requires a minute dataset");
    history_files::verify_daily_result(record.daily(), record.daily_result());
    if (record.daily_result().manifest_sha256() != query.dataset_id())
      throw std::invalid_argument("dataset archive changed during resolution");
    auto input = record.daily();
    const auto begin = std::chrono::year_month_day(
        std::chrono::sys_days(parse_trading_date(input.end_day())) + std::chrono::days(1));
    input.set_begin_day(format_trading_date(begin));
    input.set_end_day(query.end_day());
    if (input.begin_day() > input.end_day())
      throw std::invalid_argument("no later download range is requested");
    input.set_requests_per_minute(query.requests_per_minute());
    (void)history_files::daily_range(input);
    *plan.mutable_daily() = input;
  } else {
    throw std::invalid_argument("invalid history update source");
  }
  plan.set_id(sha256_bytes(plan.SerializeAsString()));
  return plan;
}
} // namespace asterion::data
