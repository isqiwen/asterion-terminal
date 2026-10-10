#include "bar_dataset_source.hpp"
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
  std::map<std::string, std::int64_t> acquisition;
  std::map<std::string, std::optional<Decimal>> settlements;
  std::optional<HistorySemantics> settlement_semantics, source_semantics;
  for (std::size_t i = 0; i < in.settlements.size(); ++i) {
    const auto stored = daily(in.settlements[i], in.request.settlement_dataset_ids(i));
    same_semantics(settlement_semantics, stored.semantics);
    acquisition.emplace(stored.info.manifest_sha256, stored.info.acquired_at_ns);
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
  result.set_version(2);
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
// Open interest by trading day from the month's daily downloads, which
// resolve_bar_dataset has verified.
std::map<std::string, Decimal> open_interest(const BarDatasetSources& in) {
  std::map<std::string, Decimal> result;
  for (const auto& record : in.settlements)
    for (const auto& bar :
         history_files::read_daily(directory(record.daily_result().directory())).bars)
      result.insert_or_assign(format_trading_date(bar.trading_day), bar.open_interest);
  return result;
}
} // namespace
DominantSeries resolve_dominant_series(const std::vector<BarDatasetSources>& inputs) {
  if (inputs.size() < 2)
    throw std::invalid_argument("a dominant series requires at least two month contracts");
  std::vector<Month> months;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    // Verify one complete selection at a time. The schedule needs daily facts,
    // not every month's full minute payload retained together.
    const auto dataset = resolve_bar_dataset(inputs[i]);
    Month month{i,  dataset.contract(),      dataset.interval_minutes(), dataset.source(),
                {}, open_interest(inputs[i])};
    for (const auto& day : dataset.days())
      month.settlement.emplace(day.trading_day(),
                               Decimal::from_raw(day.settlement_price().units()));
    months.push_back(std::move(month));
  }
  std::ranges::sort(months, {}, [](const Month& month) { return month.contract.delivery_month(); });
  std::set<std::string> all;
  for (std::size_t i = 0; i < months.size(); ++i) {
    const auto& contract = months[i].contract;
    const auto& first = months.front().contract;
    if (contract.venue() != first.venue() || contract.product() != first.product() ||
        contract.price_increment().units() != first.price_increment().units() ||
        contract.multiplier().units() != first.multiplier().units() ||
        months[i].interval != months.front().interval || months[i].source != months.front().source)
      throw std::invalid_argument(
          "a dominant series requires months of one product with the same terms and bars");
    if (i && contract.delivery_month() == months[i - 1].contract.delivery_month())
      throw std::invalid_argument("a dominant series lists each month once");
    for (const auto& [day, price] : months[i].settlement)
      all.insert(day);
  }
  const std::vector<std::string> days(all.begin(), all.end());
  if (days.size() < 2)
    throw std::invalid_argument("a dominant series requires at least two trading days");
  struct Segment {
    std::size_t month;
    std::string begin;
    // The trading day before `begin`, on which the month that takes over
    // already settled: its bars of that day give it a price to take over at.
    std::string before;
    Decimal ratio; // New over old settlement on the day before the roll.
  };
  std::vector<Segment> segments;
  std::vector<data::v1::TermPoint> terms;
  for (std::size_t d = 1; d < days.size(); ++d) {
    const auto &day = days[d], &previous = days[d - 1];
    const auto first = segments.empty() ? 0 : segments.back().month;
    std::optional<std::size_t> chosen;
    for (std::size_t m = first; m < months.size(); ++m) {
      const auto interest = months[m].open_interest.find(previous);
      if (!months[m].settlement.contains(day) || interest == months[m].open_interest.end())
        continue;
      if (!chosen || interest->second > months[*chosen].open_interest.at(previous))
        chosen = m;
    }
    if (!chosen)
      throw std::invalid_argument("no month contract of the series has data on " + day +
                                  " and open interest on " + previous);
    // The term structure the day begins with: the dominant month against the
    // later month most held the day before, where both settled then.
    const auto& dominant = months[*chosen];
    std::optional<std::size_t> later;
    for (std::size_t m = *chosen + 1; m < months.size(); ++m) {
      const auto interest = months[m].open_interest.find(previous);
      if (!months[m].settlement.contains(previous) || interest == months[m].open_interest.end())
        continue;
      if (!later || interest->second > months[*later].open_interest.at(previous))
        later = m;
    }
    if (later && dominant.settlement.contains(previous)) {
      auto& term = terms.emplace_back();
      term.set_trading_day(day);
      term.mutable_near()->set_units(dominant.settlement.at(previous).raw());
      term.mutable_far()->set_units(months[*later].settlement.at(previous).raw());
      term.set_far_month(months[*later].contract.delivery_month());
    }
    if (!segments.empty() && *chosen == segments.back().month)
      continue;
    Decimal ratio = Decimal::parse("1");
    if (!segments.empty()) {
      const auto& old = months[segments.back().month];
      const auto& symbol = old.contract.symbol();
      if (!old.settlement.contains(day))
        throw std::invalid_argument("the outgoing month has no bars on the roll day: " + symbol +
                                    " on " + day);
      if (!old.settlement.contains(previous) || !months[*chosen].settlement.contains(previous))
        throw std::invalid_argument(
            "the roll lacks both settlement prices on the previous day: " + symbol + " on " + day);
      ratio = divide(months[*chosen].settlement.at(previous), old.settlement.at(previous),
                     Rounding::half_up);
      if (ratio <= Decimal{})
        throw std::invalid_argument("invalid roll adjustment ratio on " + day);
    }
    segments.push_back({*chosen, day, previous, ratio});
  }
  DominantSeries result;
  // Back-adjustment: the last month keeps its prices; each earlier month is
  // scaled by the ratios of every roll after it.
  std::vector<Decimal> factors(segments.size(), Decimal::parse("1"));
  for (std::size_t i = segments.size() - 1; i > 0; --i)
    factors[i - 1] = multiply(factors[i], segments[i].ratio, Rounding::half_up);
  std::size_t total_bars = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& month = months[segments[i].month];
    auto end = days.back();
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
    auto dataset = resolve_bar_dataset(selected);
    total_bars += static_cast<std::size_t>(dataset.bars_size());
    if (total_bars > protocol::max_dataset_bars)
      throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
    result.months.push_back(month.input);
    result.datasets.push_back(std::move(dataset));
    auto* roll = result.schedule.add_rolls();
    roll->set_trading_day(segments[i].begin);
    roll->set_contract(static_cast<unsigned>(i));
    roll->mutable_factor()->set_units(factors[i].raw());
    if (factors[i] <= Decimal{})
      throw std::invalid_argument("invalid roll adjustment ratio on " + segments[i].begin);
  }
  for (auto& term : terms)
    *result.schedule.add_terms() = std::move(term);
  return result;
}
} // namespace asterion::data
