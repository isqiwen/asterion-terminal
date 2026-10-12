#include "bar_dataset_source.hpp"
#include "dominant_plan.hpp"
#include "table_series.hpp"
#include "history_minutes.hpp"
#include "history_metadata.hpp"
#include <asterion/domain/daily_bars.hpp>
#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
namespace asterion::data {
namespace {
using namespace std::chrono;
using Bars = std::map<std::int64_t, MarketBar>;
std::filesystem::path directory(const std::string& text) {
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::int64_t close_label(const std::string& day) {
  // 15:00 Asia/Shanghai = 07:00 UTC on the trading day.
  return duration_cast<nanoseconds>(
             (sys_days(parse_trading_date(day)) + hours(7)).time_since_epoch())
      .count();
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
// Completes a dataset from its bars: the settlement price of each of their
// days, the days of [first, last] the contract settled on without bars, and
// when the daily partitions it speaks of were acquired.
void settle(data::v1::BarDataset& result, const Bars& bars,
            const std::map<std::string, Decimal>& settlements,
            const std::map<std::string, std::int64_t>& acquired, const std::string& first,
            const std::string& last, const std::string& contract) {
  // The days the contract settled on are the trading days it had. This detects
  // missing whole days only; it cannot establish intraday completeness.
  std::set<std::string> covered;
  for (const auto& [timestamp, bar] : bars)
    covered.insert(bar.trading_day);
  for (auto day = settlements.lower_bound(first); day != settlements.end() && day->first <= last;
       ++day)
    if (!covered.contains(day->first))
      result.add_uncovered_days(day->first);
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
  // Of the days this dataset says anything about, so that the same selection
  // read again over exactly those days records the same time.
  auto from = result.days(0).trading_day(), to = result.days(result.days_size() - 1).trading_day();
  if (result.uncovered_days_size()) {
    from = std::min(from, result.uncovered_days(0));
    to = std::max(to, result.uncovered_days(result.uncovered_days_size() - 1));
  }
  std::int64_t read = 0;
  for (auto day = acquired.lower_bound(from); day != acquired.end() && day->first <= to; ++day)
    read = std::max(read, day->second);
  result.set_daily_acquired_at_ns(read);
}
template <class Values>
const Values& of(const std::map<std::string, Values>& all, const std::string& contract) {
  static const Values none;
  const auto found = all.find(contract);
  return found == all.end() ? none : found->second;
}
// The daily bars of one contract over [begin, end], as `facts` read them from
// the daily table: a day is a bar where the contract traded.
data::v1::BarDataset tabled(const protocol::v1::Contract& spec, const data::v1::DailyTable& daily,
                            const DailyFacts& facts, const std::string& contract,
                            const std::string& begin, const std::string& end) {
  Bars bars;
  const auto& trades = of(facts.trades, contract);
  for (auto day = trades.lower_bound(begin); day != trades.end() && day->first <= end; ++day) {
    const auto label = close_label(day->first);
    bars.emplace(label, MarketBar{day->first, label, day->second.open, day->second.high,
                                  day->second.low, day->second.close, day->second.volume});
  }
  if (bars.empty())
    throw std::invalid_argument("no bars fall inside the requested trading days");
  data::v1::BarDataset result;
  result.set_version(protocol::bar_dataset_version);
  *result.mutable_contract() = spec;
  settle(result, bars, of(facts.settlement, contract), facts.acquired_at_ns, begin, end, contract);
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
  const auto identity = history_identity(in.request.contract());
  const auto contract = identity.key();
  const auto begin = in.request.begin_day(), end = in.request.end_day();
  // Without archive versions the bars are the daily table's own.
  if (in.sources.empty())
    return tabled(
        in.request.contract(), in.request.daily(),
        daily_facts(tables, in.request.daily(), identity.venue, begin, end, {contract}, true),
        contract, begin, end);
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
  const auto facts =
      daily_facts(tables, in.request.daily(), identity.venue, first, last, {contract});
  *result.mutable_contract() = in.request.contract();
  settle(result, bars, of(facts.settlement, contract), facts.acquired_at_ns, first, last, contract);
  result.set_interval_minutes(*interval);
  result.set_source(source_semantics->source);
  *result.mutable_source_dataset_ids() = in.request.source_dataset_ids();
  *result.mutable_daily() = in.request.daily();
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
DominantSeries resolve_product_series(const history_files::TableStore& tables,
                                      const data::v1::ProductBarsRequest& request) {
  protocol::validate_product_bars_request(request);
  // Everything the table says of the product over the range is read once.
  const auto prefix = request.venue() + "/" + request.product() + "/";
  const auto facts = daily_facts(tables, request.daily(), request.venue(), request.begin_day(),
                                 request.end_day(), {prefix}, true);
  struct Traded {
    std::string name;
    protocol::v1::Contract contract;
    // On the days it has bars: a month has data where it can be traded.
    std::map<std::string, Decimal> settlement;
  };
  // The months that traded in the range, in delivery order: names of one
  // product sort so.
  std::vector<Traded> months;
  for (const auto& [name, trades] : facts.trades) {
    if (trades.empty())
      continue;
    const auto identity = HistoryIdentity::parse(name);
    const auto id = identity.exchange_id();
    Traded month{name, {}, {}};
    month.contract.set_venue(id.venue);
    month.contract.set_symbol(id.symbol);
    month.contract.set_currency("CNY");
    *month.contract.mutable_price_increment() = request.price_increment();
    month.contract.mutable_quantity_increment()->set_units(Decimal::parse("1").raw());
    *month.contract.mutable_multiplier() = request.multiplier();
    month.contract.set_product(id.symbol.substr(0, identity.product.size()));
    month.contract.set_delivery_month(identity.delivery_month);
    const auto& settled = of(facts.settlement, name);
    for (const auto& [day, trade] : trades)
      if (const auto price = settled.find(day); price != settled.end())
        month.settlement.emplace(day, price->second);
    months.push_back(std::move(month));
  }
  if (months.size() < 2)
    throw std::invalid_argument("a dominant series requires at least two month contracts");
  std::vector<DominantMonth> dominant;
  for (const auto& month : months)
    dominant.push_back({month.contract.symbol(), month.contract.delivery_month(), month.settlement,
                        of(facts.open_interest, month.name)});
  // Backtests trade these months: a position is closed on the roll day.
  const auto plan = plan_dominant(dominant, RollDay::must_settle);
  const auto& segments = plan.segments;
  DominantSeries result;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const auto& month = months[segments[i].month];
    // A month that is left stays until it stops trading. Its position is
    // closed at a limit, and a day has one bar to reach it: the roll day and
    // the day after are too few when the price runs away from it.
    const auto end = i + 1 < segments.size() ? month.settlement.rbegin()->first : plan.days.back();
    // The first month starts the series; a later one begins a day early.
    result.months.push_back(segments[i].month);
    result.datasets.push_back(tabled(month.contract, request.daily(), facts, month.name,
                                     i ? segments[i].before : segments[i].begin, end));
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
