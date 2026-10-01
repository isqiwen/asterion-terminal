#include <set>
#include "bar_dataset_source.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <stdexcept>
namespace asterion::tasks {
namespace {
namespace wire = research::v1;
using namespace std::chrono;
std::filesystem::path directory(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::int64_t close_label(year_month_day day) {
  // 15:00 Asia/Shanghai = 07:00 UTC on the trading day.
  return duration_cast<nanoseconds>((sys_days(day) + hours(7)).time_since_epoch()).count();
}
} // namespace
data::v1::BarDataset resolve_bar_dataset(const BarDatasetSources& in) {
  protocol::decode_bar_dataset_request(in.request);
  const auto spec = protocol::instrument(in.request.contract());
  auto product = in.request.contract().product();
  std::ranges::transform(product, product.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const HistoryIdentity identity{spec.id.venue, product, in.request.contract().delivery_month()};
  identity.validate();
  if (in.settlement.kind() != wire::DAILY_DOWNLOAD || in.settlement.state() != wire::SUCCEEDED ||
      !in.settlement.has_daily())
    throw std::invalid_argument("settlement source must be a completed daily download");
  if (history_files::daily_range(in.settlement.daily()).instrument != identity)
    throw std::invalid_argument("settlement download is for a different contract");
  history_files::verify_daily_result(in.settlement.daily(), in.settlement_result);
  const auto settled = history_files::read_daily(directory(in.settlement_result.directory()));
  if (!settled.info.complete ||
      settled.info.manifest_sha256 != in.settlement_result.manifest_sha256())
    throw std::invalid_argument("settlement dataset changed or is incomplete");
  std::map<std::string, std::optional<Decimal>> settlements;
  for (const auto& bar : settled.bars)
    settlements.emplace(format_trading_date(bar.trading_day), bar.settlement);
  const auto begin = in.request.begin_day(), end = in.request.end_day();
  const auto inside = [&](const std::string& day) {
    return (begin.empty() || day >= begin) && (end.empty() || day <= end);
  };
  std::vector<MarketBar> bars;
  const auto keep = [&](MarketBar bar) {
    if (!inside(bar.trading_day))
      return;
    if (bars.size() == protocol::max_dataset_bars)
      throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
    bars.push_back(std::move(bar));
  };
  data::v1::BarDataset result;
  result.set_version(1);
  if (in.source.state() != wire::SUCCEEDED)
    throw std::invalid_argument("bar source download has not completed");
  if (in.source.kind() == wire::MINUTE_DOWNLOAD && in.source.has_minutes() && in.minutes) {
    const auto range = history_files::minute_range(in.source.minutes());
    if (range.instrument != identity)
      throw std::invalid_argument("minute download is for a different contract");
    history_files::verify_minute_result(in.source.minutes(), *in.minutes);
    if (history_files::minute_semantics(directory(in.minutes->directory())).timestamp_semantics !=
        "bar_end")
      throw std::invalid_argument("historical minute label semantics are unavailable for research");
    history_files::read_minutes(directory(in.minutes->directory()), [&](const HistoricalBar& bar) {
      if (bar.trading_day.empty())
        throw std::invalid_argument(
            "historical minute trading day is unavailable; use daily data for research");
      if (!settlements.contains(bar.trading_day))
        throw std::invalid_argument("settlement is missing for the minute trading day");
      keep({bar.trading_day, bar.timestamp_ns, bar.open, bar.high, bar.low, bar.close, bar.volume});
    });
    result.set_interval_minutes(in.source.minutes().interval_minutes());
    result.set_source(in.source.minutes().source());
    result.set_manifest_sha256(in.minutes->manifest_sha256());
  } else if (in.source.kind() == wire::DAILY_DOWNLOAD && in.source.has_daily() && in.daily) {
    if (history_files::daily_range(in.source.daily()).instrument != identity)
      throw std::invalid_argument("daily download is for a different contract");
    history_files::verify_daily_result(in.source.daily(), *in.daily);
    const auto stored = history_files::read_daily(directory(in.daily->directory()));
    if (!stored.info.complete || stored.info.manifest_sha256 != in.daily->manifest_sha256())
      throw std::invalid_argument("daily dataset changed or is incomplete");
    for (const auto& bar : stored.bars)
      keep({format_trading_date(bar.trading_day), close_label(bar.trading_day), bar.open, bar.high,
            bar.low, bar.close, bar.volume});
    result.set_interval_minutes(0);
    result.set_source(in.source.daily().source());
    result.set_manifest_sha256(in.daily->manifest_sha256());
  } else {
    throw std::invalid_argument("bar source must be a completed minute or daily download");
  }
  if (bars.empty())
    throw std::invalid_argument("no bars fall inside the requested trading days");
  if (in.source.kind() == wire::MINUTE_DOWNLOAD) {
    // Exchange trading days from the daily download that have no minute bars.
    std::set<std::string> covered;
    for (const auto& bar : bars)
      covered.insert(bar.trading_day);
    for (const auto& [day, price] : settlements)
      if (day > bars.front().trading_day && day < bars.back().trading_day && inside(day) &&
          !covered.contains(day))
        result.add_uncovered_days(day);
  }
  *result.mutable_contract() = in.request.contract();
  for (const auto& bar : bars)
    *result.add_bars() = protocol::encode_bar(bar);
  for (const auto& bar : bars) {
    if (result.days_size() && result.days(result.days_size() - 1).trading_day() == bar.trading_day)
      continue;
    const auto& price = settlements.at(bar.trading_day);
    if (!price)
      throw std::invalid_argument("daily data has no settlement price for " + bar.trading_day);
    auto* day = result.add_days();
    day->set_trading_day(bar.trading_day);
    day->mutable_settlement_price()->set_units(price->raw());
  }
  result.set_source_task_id(in.source.id());
  result.set_settlement_task_id(in.settlement.id());
  result.set_settlement_manifest_sha256(in.settlement_result.manifest_sha256());
  result.set_revision(protocol::bar_dataset_revision(result));
  protocol::validate_bar_dataset(result);
  return result;
}
} // namespace asterion::tasks
