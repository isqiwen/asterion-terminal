#include "bar_dataset_source.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <stdexcept>
namespace asterion::tasks {
namespace {
using namespace std::chrono;
std::filesystem::path directory(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::int64_t close_label(year_month_day day) {
  // 15:00 Asia/Shanghai = 07:00 UTC on the trading day.
  return duration_cast<nanoseconds>((sys_days(day) + hours(7)).time_since_epoch()).count();
}
void same_semantics(std::optional<HistorySemantics>& expected, const HistorySemantics& actual) {
  if (expected && *expected != actual)
    throw std::invalid_argument("dataset versions have different data semantics");
  expected = actual;
}
} // namespace
data::v1::BarDataset resolve_bar_dataset(const BarDatasetSources& in) {
  protocol::decode_bar_dataset_request(in.request);
  if (in.sources.size() != static_cast<std::size_t>(in.request.source_dataset_ids_size()) ||
      in.settlements.size() != static_cast<std::size_t>(in.request.settlement_dataset_ids_size()))
    throw std::invalid_argument("dataset archive changed during resolution");
  const auto spec = protocol::instrument(in.request.contract());
  auto product = in.request.contract().product();
  std::ranges::transform(product, product.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const HistoryIdentity identity{spec.id.venue, product, in.request.contract().delivery_month()};
  identity.validate();
  const auto begin = in.request.begin_day(), end = in.request.end_day();
  const auto inside = [&](const std::string& day) {
    return (begin.empty() || day >= begin) && (end.empty() || day <= end);
  };
  const auto daily = [&](const data::v1::HistoryRecord& record, const std::string& id) {
    if (!record.has_daily() || !record.has_daily_result())
      throw std::invalid_argument("settlement source must be a completed daily download");
    if (history_files::daily_range(record.daily()).instrument != identity)
      throw std::invalid_argument("daily download is for a different contract");
    history_files::verify_daily_result(record.daily(), record.daily_result());
    auto stored = history_files::read_daily(directory(record.daily_result().directory()));
    if (!stored.info.complete || stored.info.manifest_sha256 != id ||
        stored.info.manifest_sha256 != record.daily_result().manifest_sha256())
      throw std::invalid_argument("daily dataset changed or is incomplete");
    return stored;
  };
  std::map<std::string, std::optional<Decimal>> settlements;
  std::optional<HistorySemantics> settlement_semantics, source_semantics;
  for (std::size_t i = 0; i < in.settlements.size(); ++i) {
    const auto stored = daily(in.settlements[i], in.request.settlement_dataset_ids(i));
    same_semantics(settlement_semantics, stored.semantics);
    for (const auto& bar : stored.bars) {
      const auto day = format_trading_date(bar.trading_day);
      if (!inside(day))
        continue;
      const auto [at, inserted] = settlements.emplace(day, bar.settlement);
      if (!inserted && at->second != bar.settlement)
        throw std::invalid_argument("dataset versions contain conflicting settlement prices");
    }
  }
  std::map<std::int64_t, MarketBar> bars;
  const auto keep = [&](MarketBar bar) {
    if (!inside(bar.trading_day))
      return;
    const auto [at, inserted] = bars.emplace(bar.timestamp_ns, bar);
    if (!inserted && protocol::encode_bar(at->second).SerializeAsString() !=
                         protocol::encode_bar(bar).SerializeAsString())
      throw std::invalid_argument("dataset versions contain conflicting bars");
    if (bars.size() > protocol::max_dataset_bars)
      throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
  };
  data::v1::BarDataset result;
  result.set_version(1);
  std::optional<unsigned> interval;
  for (std::size_t i = 0; i < in.sources.size(); ++i) {
    const auto& source = in.sources[i];
    const auto next_interval = source.has_minutes() ? source.minutes().interval_minutes() : 0;
    if (interval && *interval != next_interval)
      throw std::invalid_argument("dataset versions have different bar intervals");
    interval = next_interval;
    if (source.has_minutes() && source.has_minute_result()) {
      if (history_files::minute_range(source.minutes()).instrument != identity)
        throw std::invalid_argument("minute download is for a different contract");
      history_files::verify_minute_result(source.minutes(), source.minute_result());
      if (source.minute_result().manifest_sha256() != in.request.source_dataset_ids(i))
        throw std::invalid_argument("dataset archive changed during resolution");
      const auto path = directory(source.minute_result().directory());
      const auto semantics = history_files::minute_semantics(path);
      if (semantics.timestamp_semantics != "bar_end")
        throw std::invalid_argument(
            "historical minute label semantics are unavailable for research");
      same_semantics(source_semantics, semantics);
      history_files::read_minutes(path, [&](const HistoricalBar& bar) {
        if (bar.trading_day.empty())
          throw std::invalid_argument(
              "historical minute trading day is unavailable; use daily data for research");
        keep({bar.trading_day, bar.timestamp_ns, bar.open, bar.high, bar.low, bar.close,
              bar.volume});
      });
    } else if (source.has_daily() && source.has_daily_result()) {
      const auto stored = daily(source, in.request.source_dataset_ids(i));
      same_semantics(source_semantics, stored.semantics);
      for (const auto& bar : stored.bars)
        keep({format_trading_date(bar.trading_day), close_label(bar.trading_day), bar.open,
              bar.high, bar.low, bar.close, bar.volume});
    } else {
      throw std::invalid_argument("bar source must be a completed minute or daily download");
    }
  }
  if (bars.empty())
    throw std::invalid_argument("no bars fall inside the requested trading days");
  // Daily settlement rows are the available trading calendar. This detects
  // missing whole days only; it cannot establish intraday completeness.
  std::set<std::string> covered;
  for (const auto& [timestamp, bar] : bars)
    covered.insert(bar.trading_day);
  for (const auto& [day, price] : settlements)
    if (day >= (begin.empty() ? bars.begin()->second.trading_day : begin) &&
        day <= (end.empty() ? bars.rbegin()->second.trading_day : end) && !covered.contains(day))
      result.add_uncovered_days(day);
  *result.mutable_contract() = in.request.contract();
  for (const auto& [timestamp, bar] : bars) {
    *result.add_bars() = protocol::encode_bar(bar);
    if (result.days_size() && result.days(result.days_size() - 1).trading_day() == bar.trading_day)
      continue;
    if (!settlements.contains(bar.trading_day))
      throw std::invalid_argument("settlement is missing for the minute trading day");
    const auto& price = settlements.at(bar.trading_day);
    if (!price)
      throw std::invalid_argument("daily data has no settlement price for " + bar.trading_day);
    auto* day = result.add_days();
    day->set_trading_day(bar.trading_day);
    day->mutable_settlement_price()->set_units(price->raw());
  }
  result.set_interval_minutes(*interval);
  result.set_source(source_semantics->source);
  *result.mutable_source_dataset_ids() = in.request.source_dataset_ids();
  *result.mutable_settlement_dataset_ids() = in.request.settlement_dataset_ids();
  result.set_revision(protocol::bar_dataset_revision(result));
  protocol::validate_bar_dataset(result);
  return result;
}
} // namespace asterion::tasks
