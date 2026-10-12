#include "bar_dataset_source.hpp"
#include "dominant_plan.hpp"
#include "table_series.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include "history_metadata.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
namespace asterion::data {
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
// The contract as history names it, which is how the daily table keys its rows.
HistoryIdentity history_identity(const protocol::v1::Contract& contract) {
  auto product = contract.product();
  std::ranges::transform(product, product.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const HistoryIdentity identity{protocol::instrument(contract).id.venue, product,
                                 contract.delivery_month()};
  identity.validate();
  return identity;
}
} // namespace
data::v1::BarDataset resolve_bar_dataset(const history_files::TableStore& tables,
                                         const BarDatasetSources& in) {
  protocol::decode_bar_dataset_request(in.request);
  if (in.sources.size() != static_cast<std::size_t>(in.request.source_dataset_ids_size()))
    throw std::invalid_argument("dataset archive changed during resolution");
  const auto identity = history_identity(in.request.contract());
  const auto begin = in.request.begin_day(), end = in.request.end_day();
  const auto inside = [&](const std::string& day) {
    return (begin.empty() || day >= begin) && (end.empty() || day <= end);
  };
  const auto daily = [&](const data::v1::HistoryRecord& record, const std::string& id) {
    if (history_files::daily_range(record.daily()).instrument != identity)
      throw std::invalid_argument("daily download is for a different contract");
    history_files::verify_daily_result(record.daily(), record.daily_result());
    auto stored = history_files::read_daily(directory(record.daily_result().directory()));
    if (!stored.info.complete || stored.info.manifest_sha256 != id ||
        stored.info.manifest_sha256 != record.daily_result().manifest_sha256())
      throw std::invalid_argument("daily dataset changed or is incomplete");
    return stored;
  };
  std::map<std::string, std::int64_t> acquisition;
  std::optional<HistorySemantics> source_semantics;
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
  result.set_version(protocol::bar_dataset_version);
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
      const auto metadata = history_files::inspect_minutes(path);
      const auto semantics = history_files::decode_semantics(metadata.at("semantics"));
      acquisition.emplace(source.minute_result().manifest_sha256(),
                          history_files::acquired_at(metadata));
      if (semantics.timestamp_semantics != "bar_end")
        throw std::invalid_argument("historical minute label semantics are unavailable for task");
      same_semantics(source_semantics, semantics);
      history_files::read_minutes(path, [&](const HistoricalBar& bar) {
        if (bar.trading_day.empty())
          throw std::invalid_argument(
              "historical minute trading day is unavailable; use daily data for task");
        keep({bar.trading_day, bar.timestamp_ns, bar.open, bar.high, bar.low, bar.close,
              bar.volume});
      });
    } else if (source.has_daily() && source.has_daily_result()) {
      const auto stored = daily(source, in.request.source_dataset_ids(i));
      same_semantics(source_semantics, stored.semantics);
      acquisition.emplace(stored.info.manifest_sha256, stored.info.acquired_at_ns);
      for (const auto& bar : stored.bars)
        keep({format_trading_date(bar.trading_day), close_label(bar.trading_day), bar.open,
              bar.high, bar.low, bar.close, bar.volume});
    } else {
      throw std::invalid_argument("bar source must be a completed minute or daily download");
    }
  }
  if (bars.empty())
    throw std::invalid_argument("no bars fall inside the requested trading days");
  // The days the contract settled on are the trading days it had. This detects
  // missing whole days only; it cannot establish intraday completeness.
  const auto contract = identity.key();
  const auto facts = daily_facts(tables, in.request.daily(), identity.venue,
                                 begin.empty() ? bars.begin()->second.trading_day : begin,
                                 end.empty() ? bars.rbegin()->second.trading_day : end, {contract});
  const auto settled = facts.settlement.find(contract);
  const auto settlements =
      settled == facts.settlement.end() ? std::map<std::string, Decimal>{} : settled->second;
  std::set<std::string> covered;
  for (const auto& [timestamp, bar] : bars)
    covered.insert(bar.trading_day);
  for (const auto& [day, price] : settlements)
    if (!covered.contains(day))
      result.add_uncovered_days(day);
  *result.mutable_contract() = in.request.contract();
  for (const auto& [timestamp, bar] : bars) {
    *result.add_bars() = protocol::encode_bar(bar);
    if (result.days_size() && result.days(result.days_size() - 1).trading_day() == bar.trading_day)
      continue;
    const auto price = settlements.find(bar.trading_day);
    if (price == settlements.end())
      throw std::invalid_argument("futures.daily has no settlement price for " + contract + " on " +
                                  bar.trading_day);
    auto* day = result.add_days();
    day->set_trading_day(bar.trading_day);
    day->mutable_settlement_price()->set_units(price->second.raw());
  }
  result.set_interval_minutes(*interval);
  result.set_source(source_semantics->source);
  *result.mutable_source_dataset_ids() = in.request.source_dataset_ids();
  *result.mutable_daily() = in.request.daily();
  // Of the days this dataset says anything about, so that the same selection
  // read again over exactly those days records the same time.
  auto first = result.days(0).trading_day(),
       last = result.days(result.days_size() - 1).trading_day();
  if (result.uncovered_days_size()) {
    first = std::min(first, result.uncovered_days(0));
    last = std::max(last, result.uncovered_days(result.uncovered_days_size() - 1));
  }
  std::int64_t read = 0;
  for (auto day = facts.acquired_at_ns.lower_bound(first);
       day != facts.acquired_at_ns.end() && day->first <= last; ++day)
    read = std::max(read, day->second);
  result.set_daily_acquired_at_ns(read);
  for (const auto& [id, time] : acquisition) {
    auto* evidence = result.add_history_evidence();
    evidence->set_dataset_id(id);
    evidence->set_acquired_at_ns(time);
    evidence->set_source_availability(data::v1::SOURCE_AVAILABILITY_UNKNOWN);
  }
  result.set_revision(protocol::bar_dataset_revision(result));
  protocol::validate_bar_dataset(result);
  return result;
}
namespace {
struct Month {
  std::size_t input;
  protocol::v1::Contract contract;
  unsigned interval;
  std::string source;
  std::map<std::string, Decimal> settlement, open_interest;
};
} // namespace
DominantSeries resolve_dominant_series(const history_files::TableStore& tables,
                                       const std::vector<BarDatasetSources>& inputs) {
  if (inputs.size() < 2)
    throw std::invalid_argument("a dominant series requires at least two month contracts");
  std::vector<Month> months;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    // Verify one complete selection at a time. The schedule needs daily facts,
    // not every month's full minute payload retained together.
    const auto dataset = resolve_bar_dataset(tables, inputs[i]);
    Month month{i, dataset.contract(), dataset.interval_minutes(), dataset.source(), {}, {}};
    for (const auto& day : dataset.days())
      month.settlement.emplace(day.trading_day(),
                               Decimal::from_raw(day.settlement_price().units()));
    months.push_back(std::move(month));
  }
  std::ranges::sort(months, {}, [](const Month& month) { return month.contract.delivery_month(); });
  const auto& daily = inputs.front().request.daily();
  for (std::size_t i = 0; i < months.size(); ++i) {
    const auto& contract = months[i].contract;
    const auto& first = months.front().contract;
    if (contract.venue() != first.venue() || contract.product() != first.product() ||
        contract.price_increment().units() != first.price_increment().units() ||
        contract.multiplier().units() != first.multiplier().units() ||
        months[i].interval != months.front().interval ||
        months[i].source != months.front().source ||
        inputs[i].request.daily().SerializeAsString() != daily.SerializeAsString())
      throw std::invalid_argument(
          "a dominant series requires months of one product with the same terms and bars");
  }
  // What every month held, on each day any of them has bars: a month may lead
  // on the first day of its own bars, by what it held the day before.
  std::vector<std::string> names;
  auto first_day = months.front().settlement.begin()->first, last_day = first_day;
  for (const auto& month : months) {
    names.push_back(history_identity(month.contract).key());
    first_day = std::min(first_day, month.settlement.begin()->first);
    last_day = std::max(last_day, month.settlement.rbegin()->first);
  }
  auto held =
      daily_facts(tables, daily, months.front().contract.venue(), first_day, last_day, names)
          .open_interest;
  std::vector<DominantMonth> facts;
  for (std::size_t i = 0; i < months.size(); ++i) {
    months[i].open_interest = std::move(held[names[i]]);
    facts.push_back({months[i].contract.symbol(), months[i].contract.delivery_month(),
                     months[i].settlement, months[i].open_interest});
  }
  // Backtests trade these months: a position is closed on the roll day.
  const auto plan = plan_dominant(facts, RollDay::must_settle);
  const auto& segments = plan.segments;
  DominantSeries result;
  std::size_t total_bars = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& month = months[segments[i].month];
    auto end = plan.days.back();
    if (i + 1 < segments.size()) {
      // Through the roll day and the month's next trading day after it.
      end = segments[i + 1].begin;
      const auto next = month.settlement.upper_bound(end);
      if (next != month.settlement.end())
        end = next->first;
    }
    auto selected = inputs[month.input];
    // The first month starts the series; a later one begins a day early.
    selected.request.set_begin_day(i ? segments[i].before : segments[i].begin);
    selected.request.set_end_day(end);
    auto dataset = resolve_bar_dataset(tables, selected);
    total_bars += static_cast<std::size_t>(dataset.bars_size());
    if (total_bars > protocol::max_dataset_bars)
      throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
    result.months.push_back(month.input);
    result.datasets.push_back(std::move(dataset));
    auto* roll = result.schedule.add_rolls();
    roll->set_trading_day(segments[i].begin);
    roll->set_contract(static_cast<unsigned>(i));
    roll->mutable_factor()->set_units(segments[i].factor.raw());
  }
  for (const auto& term : plan.terms)
    *result.schedule.add_terms() = term;
  return result;
}
} // namespace asterion::data
