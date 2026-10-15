#include "bar_dataset_source.hpp"
#include "dominant_plan.hpp"
#include "table_series.hpp"
#include "history_minutes.hpp"
#include "history_metadata.hpp"
#include <asterion/domain/trading_days.hpp>
#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
namespace asterion::data {
namespace {
using Bars = std::map<std::int64_t, MarketBar>;
std::filesystem::path directory(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
void same_semantics(std::optional<HistorySemantics>& expected, const HistorySemantics& actual) {
  if (expected && *expected != actual)
    throw std::invalid_argument("dataset versions have different data semantics");
  expected = actual;
}
template <class Values>
const Values& of(const std::map<std::string, Values>& all, const std::string& contract) {
  static const Values none;
  const auto found = all.find(contract);
  return found == all.end() ? none : found->second;
}
// The contract of a dataset with the units the data give it: the contract
// table's multiplier, and the price increment of each of its bars' days, as
// that of the first and the changes after it.
void set_units(data::v1::BarDataset& result, const HistoryIdentity& identity, Decimal multiplier,
               const std::map<std::string, Decimal>& increments, const Bars& bars) {
  const auto id = identity.exchange_id();
  auto& contract = *result.mutable_contract();
  contract.set_venue(id.venue);
  contract.set_symbol(id.symbol);
  contract.set_currency("CNY");
  contract.mutable_quantity_increment()->set_units(Decimal::parse("1").raw());
  contract.mutable_multiplier()->set_units(multiplier.raw());
  contract.set_product(id.symbol.substr(0, identity.product.size()));
  contract.set_delivery_month(identity.delivery_month);
  std::optional<Decimal> current;
  for (const auto& [timestamp, bar] : bars) {
    const auto increment = increments.find(bar.trading_day);
    if (increment == increments.end())
      throw std::invalid_argument("futures.daily has no auction prices of " + identity.venue + "/" +
                                  identity.product + " on " + bar.trading_day);
    if (!current)
      contract.mutable_price_increment()->set_units(increment->second.raw());
    else if (increment->second != *current) {
      auto* change = result.add_price_increment_changes();
      change->set_trading_day(bar.trading_day);
      change->mutable_value()->set_units(increment->second.raw());
    }
    current = increment->second;
  }
}
// Completes a dataset from its bars: every day of [first, last] the contract
// has a settlement price on, with bars or without, and when the daily
// partitions it speaks of were acquired. The contract table's multiplier is
// checked against what its lots were worth on those days.
void settle(data::v1::BarDataset& result, const Bars& bars, const DailyFacts& facts,
            const std::string& first, const std::string& last, const std::string& contract) {
  const auto& settlements = of(facts.settlement, contract);
  const auto& acquired = facts.acquired_at_ns;
  // What is traded is settled: a bar without a settlement price of its day
  // is not dropped, and the price of another day is not put in its place.
  for (const auto& [timestamp, bar] : bars) {
    *result.add_bars() = protocol::encode_bar(bar);
    if (!settlements.contains(bar.trading_day))
      throw std::invalid_argument("futures.daily has no settlement price for " + contract + " on " +
                                  bar.trading_day);
  }
  // A day without bars is a day all the same: the contract did not trade on
  // it, or its bars are missing, and a position held over it is settled.
  for (auto day = settlements.lower_bound(first); day != settlements.end() && day->first <= last;
       ++day) {
    auto* settled = result.add_days();
    settled->set_trading_day(day->first);
    settled->mutable_settlement_price()->set_units(day->second.raw());
  }
  // Of the days this dataset says anything about, so that the same selection
  // read again over exactly those days records the same time.
  const auto from = result.days(0).trading_day(),
             to = result.days(result.days_size() - 1).trading_day();
  std::int64_t read = 0;
  for (auto day = acquired.lower_bound(from); day != acquired.end() && day->first <= to; ++day)
    read = std::max(read, day->second);
  result.set_daily_acquired_at_ns(read);
  // A lot is worth about the day's settlement price times the multiplier. A
  // day agrees within a twentieth: turnover of a delivery month includes lots
  // that changed hands away from the day's prices. A multiplier that is wrong
  // agrees on no day, so most days must.
  const auto units = [](const protocol::v1::Decimal& value) {
    return static_cast<long double>(value.units()) / Decimal::parse("1").raw();
  };
  const auto& worth = of(facts.lot_value, contract);
  const auto multiplier = units(result.contract().multiplier());
  std::vector<long double> implied;
  for (const auto& day : result.days())
    if (const auto value = worth.find(day.trading_day()); value != worth.end())
      implied.push_back(value->second / units(day.settlement_price()));
  const auto agree = std::ranges::count_if(
      implied, [&](long double value) { return std::abs(value / multiplier - 1) <= 0.05L; });
  if (static_cast<std::size_t>(agree) * 2 < implied.size()) {
    const auto median = implied.begin() + static_cast<std::ptrdiff_t>(implied.size() / 2);
    std::ranges::nth_element(implied, median);
    throw std::invalid_argument("the contract multiplier does not match the turnover of " +
                                contract + ": its days imply about " +
                                std::format("{:.6g}", static_cast<double>(*median)));
  }
}
// The daily bars of one contract over [begin, end], as `facts` read them from
// the daily table: a day is a bar where the contract traded.
data::v1::BarDataset tabled(const HistoryIdentity& identity, Decimal multiplier,
                            const std::map<std::string, Decimal>& increments,
                            const data::v1::DailyTable& daily, const DailyFacts& facts,
                            const std::string& begin, const std::string& end) {
  const auto contract = identity.key();
  Bars bars;
  const auto& trades = of(facts.trades, contract);
  for (auto day = trades.lower_bound(begin); day != trades.end() && day->first <= end; ++day) {
    const auto label = trading_day_close_ns(day->first);
    bars.emplace(label, MarketBar{day->first, label, day->second.open, day->second.high,
                                  day->second.low, day->second.close, day->second.volume});
  }
  if (bars.empty())
    throw std::invalid_argument("no bars fall inside the requested trading days");
  data::v1::BarDataset result;
  result.set_version(protocol::bar_dataset_version);
  set_units(result, identity, multiplier, increments, bars);
  settle(result, bars, facts, begin, end, contract);
  result.set_interval_minutes(0);
  result.set_source(daily.daily_source());
  *result.mutable_daily() = daily;
  result.set_revision(protocol::bar_dataset_revision(result));
  protocol::validate_bar_dataset(result);
  return result;
}
} // namespace
data::v1::BarDataset resolve_bar_dataset(const history_files::TableStore& tables,
                                         const BarDatasetSources& in) {
  protocol::decode_bar_dataset_request(in.request);
  if (in.sources.size() != static_cast<std::size_t>(in.request.source_dataset_ids_size()))
    throw std::invalid_argument("dataset archive changed during resolution");
  const auto identity = HistoryIdentity::parse(in.request.contract());
  const auto contract = identity.key();
  const auto& daily = in.request.daily();
  const auto begin = in.request.begin_day(), end = in.request.end_day();
  const auto multiplier =
      contract_multipliers(tables, daily, identity.venue, {contract}).at(contract);
  // Without archive versions the bars are the daily table's own.
  if (in.sources.empty())
    return tabled(identity, multiplier,
                  price_increments(tables, daily, identity.venue, identity.product, begin, end),
                  daily, daily_facts(tables, daily, identity.venue, begin, end, {contract}, true),
                  begin, end);
  const auto inside = [&](const std::string& day) {
    return (begin.empty() || day >= begin) && (end.empty() || day <= end);
  };
  std::map<std::string, std::int64_t> acquisition;
  std::optional<HistorySemantics> source_semantics;
  Bars bars;
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
    // Daily bars are the daily table's; a download of them is not a source.
    if (!source.has_minutes() || !source.has_minute_result())
      throw std::invalid_argument("bar source must be a completed minute download");
    if (interval && *interval != source.minutes().interval_minutes())
      throw std::invalid_argument("dataset versions have different bar intervals");
    interval = source.minutes().interval_minutes();
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
      keep({bar.trading_day, bar.timestamp_ns, bar.open, bar.high, bar.low, bar.close, bar.volume});
    });
  }
  if (bars.empty())
    throw std::invalid_argument("no bars fall inside the requested trading days");
  const auto first = begin.empty() ? bars.begin()->second.trading_day : begin,
             last = end.empty() ? bars.rbegin()->second.trading_day : end;
  const auto facts = daily_facts(tables, daily, identity.venue, first, last, {contract});
  set_units(result, identity, multiplier,
            price_increments(tables, daily, identity.venue, identity.product, first, last), bars);
  settle(result, bars, facts, first, last, contract);
  result.set_interval_minutes(*interval);
  result.set_source(source_semantics->source);
  *result.mutable_source_dataset_ids() = in.request.source_dataset_ids();
  *result.mutable_daily() = daily;
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
  // The days it has bars on.
  std::set<std::string> bars;
};
// How long a month that is left stays in its series, by the days it has bars
// on: to its first bar from the roll day on, at whose open what it holds is
// closed, and its next after that, for what that bar's volume did not take.
// The plan has made sure of the first.
std::string left_until(const std::set<std::string>& bars, const std::string& roll_day) {
  const auto closed = bars.lower_bound(roll_day);
  const auto next = std::next(closed);
  return next == bars.end() ? *closed : *next;
}
// What a month is to the dominant rule: its settlements and open interest as
// the daily table has them, on days it traded and days it did not.
DominantMonth dominant_month(const HistoryIdentity& month, const DailyFacts& facts,
                             const std::set<std::string>& bars) {
  const auto name = month.key();
  return {month.exchange_id().symbol, month.delivery_month, of(facts.settlement, name),
          of(facts.open_interest, name), *bars.rbegin()};
}
} // namespace
DominantSeries resolve_dominant_series(const history_files::TableStore& tables,
                                       const std::vector<BarDatasetSources>& selections,
                                       const std::string& anchor) {
  auto inputs = selections;
  if (inputs.empty() || (anchor.empty() && inputs.size() < 2))
    throw std::invalid_argument("a dominant series requires at least two month contracts");
  if (!anchor.empty()) {
    const auto identity = HistoryIdentity::parse(inputs.front().request.contract());
    const auto previous =
        previous_trading_day(tables, inputs.front().request.daily(), identity.venue, anchor);
    std::string end;
    for (const auto& source : inputs)
      end = std::max(end, source.request.end_day());
    for (auto& source : inputs) {
      source.request.set_begin_day(previous);
      source.request.set_end_day(end);
    }
  }
  std::vector<Month> months;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    // Verify one complete selection at a time. The schedule needs daily facts,
    // not every month's full minute payload retained together.
    const auto dataset = resolve_bar_dataset(tables, inputs[i]);
    Month month{i, dataset.contract(), dataset.interval_minutes(), dataset.source(), {}};
    for (const auto& bar : dataset.bars())
      month.bars.insert(bar.trading_day());
    months.push_back(std::move(month));
  }
  std::ranges::sort(months, {}, [](const Month& month) { return month.contract.delivery_month(); });
  const auto& daily = inputs.front().request.daily();
  for (std::size_t i = 0; i < months.size(); ++i) {
    const auto& contract = months[i].contract;
    const auto& first = months.front().contract;
    if (contract.venue() != first.venue() || contract.product() != first.product() ||
        months[i].interval != months.front().interval ||
        months[i].source != months.front().source ||
        inputs[i].request.daily().SerializeAsString() != daily.SerializeAsString())
      throw std::invalid_argument(
          "a dominant series requires months of one product with bars of one interval and source");
  }
  // What every month settled at and held, from the first day any of them has
  // bars to the last. The table says which month is dominant and when one
  // stops trading; the bars say only where it can be traded. A month whose
  // bars end before it does is not left early for that.
  std::vector<HistoryIdentity> identities;
  std::vector<std::string> names;
  auto first_day = *months.front().bars.begin(), last_day = first_day;
  for (const auto& month : months) {
    identities.push_back(protocol::history_identity(month.contract));
    names.push_back(identities.back().key());
    first_day = std::min(first_day, *month.bars.begin());
    last_day = std::max(last_day, *month.bars.rbegin());
  }
  if (!anchor.empty()) {
    first_day = inputs.front().request.begin_day();
    last_day = inputs.front().request.end_day();
  }
  const auto table =
      daily_facts(tables, daily, months.front().contract.venue(), first_day, last_day, names);
  std::vector<DominantMonth> facts;
  for (std::size_t i = 0; i < months.size(); ++i)
    facts.push_back(dominant_month(identities[i], table, months[i].bars));
  const auto plan = plan_dominant(facts, SeriesUse::traded);
  if (!anchor.empty() && (plan.segments.empty() || plan.segments.front().begin != anchor))
    throw std::invalid_argument(
        "new snapshot cannot preserve the dominant replay anchor; configure a new backtest");
  const auto& segments = plan.segments;
  DominantSeries result;
  std::size_t total_bars = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& month = months[segments[i].month];
    auto selected = inputs[month.input];
    // The first month starts the series; a later one begins a day early.
    selected.request.set_begin_day(i ? segments[i].before : segments[i].begin);
    selected.request.set_end_day(
        i + 1 < segments.size() ? left_until(month.bars, segments[i + 1].begin) : plan.days.back());
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
DominantSeries resolve_product_series(const history_files::TableStore& tables,
                                      const data::v1::ProductBarsRequest& request) {
  protocol::validate_product_bars_request(request);
  // Everything the table says of the product over the range is read once.
  const auto prefix = request.venue() + "/" + request.product() + "/";
  const auto facts = daily_facts(tables, request.daily(), request.venue(), request.begin_day(),
                                 request.end_day(), {prefix}, true);
  struct Traded {
    HistoryIdentity identity;
    // The days it has bars on: those it traded on, and settled.
    std::set<std::string> bars;
  };
  // The months that have bars in the range, in delivery order: names of one
  // product sort so.
  std::vector<Traded> months;
  for (const auto& [name, trades] : facts.trades) {
    std::set<std::string> bars;
    const auto& settled = of(facts.settlement, name);
    for (const auto& [day, trade] : trades)
      if (settled.contains(day))
        bars.insert(day);
    if (bars.empty())
      continue;
    months.push_back({HistoryIdentity::parse(name), std::move(bars)});
  }
  if (months.size() < 2)
    throw std::invalid_argument("a dominant series requires at least two month contracts");
  std::vector<DominantMonth> dominant;
  for (const auto& month : months)
    dominant.push_back(dominant_month(month.identity, facts, month.bars));
  const auto plan = plan_dominant(dominant, SeriesUse::traded);
  const auto& segments = plan.segments;
  // The units of the months that are dominant, and the product's increments.
  std::vector<std::string> names;
  for (const auto& segment : segments)
    names.push_back(months[segment.month].identity.key());
  const auto multipliers = contract_multipliers(tables, request.daily(), request.venue(), names);
  const auto increments =
      price_increments(tables, request.daily(), request.venue(), request.product(),
                       request.begin_day(), request.end_day());
  DominantSeries result;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& month = months[segments[i].month];
    // The first month starts the series; a later one begins a day early.
    result.months.push_back(segments[i].month);
    result.datasets.push_back(
        tabled(month.identity, multipliers.at(names[i]), increments, request.daily(), facts,
               i ? segments[i].before : segments[i].begin,
               i + 1 < segments.size() ? left_until(month.bars, segments[i + 1].begin)
                                       : plan.days.back()));
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
