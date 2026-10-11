#include "table_series.hpp"
#include "dominant_plan.hpp"
#include "table_update.hpp"
#include <asterion/domain/daily_bars.hpp>
#include <asterion/domain/history_identity.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <chrono>
namespace asterion::data {
namespace {
using history_files::TableRevision;
using history_files::TableSeries;
using history_files::TableStore;
constexpr std::string_view daily_table = "futures.daily";
std::string date_text(std::int64_t day) {
  using namespace std::chrono;
  return format_trading_date(year_month_day(sys_days(days(day))));
}
TableSeries series_of(std::string_view table, const std::string& source) {
  return {std::string(table), history_table(table).schema, source};
}
std::uint64_t held(const TableStore& store, const std::string& snapshot) {
  const auto until = store.sequence(snapshot);
  if (!until)
    throw Error(ErrorCode::not_found, "the table snapshot is not in this store");
  return *until;
}
// Days of [first, last] the snapshot's calendar has open.
std::vector<std::string> open_days(const TableStore& store, const TableSeries& calendar,
                                   const std::string& venue, const std::string& first,
                                   const std::string& last, std::uint64_t until) {
  const auto& table = history_table(calendar_table);
  const auto day = table_column(table, "day"), open = table_column(table, "open");
  std::vector<std::string> result;
  for (auto year = std::stoi(first.substr(0, 4)); year <= std::stoi(last.substr(0, 4)); ++year) {
    const auto text = std::to_string(year);
    const auto revisions = store.revisions(calendar, venue, text, text, until);
    if (revisions.empty())
      throw Error(ErrorCode::conflict, "the calendar of the range is not stored: " +
                                           std::string(calendar_table) + " " + venue + " " + text);
    for (const auto& row : store.rows(revisions.front())) {
      const auto* flag = std::get_if<std::int64_t>(&row[open]);
      const auto date = date_text(std::get<std::int64_t>(row[day]));
      if (flag && *flag && date >= first && date <= last)
        result.push_back(date);
    }
  }
  std::ranges::sort(result);
  return result;
}
// What one contract did on the days of the range.
struct Month {
  std::map<std::string, Decimal> close, settlement, open_interest;
};
} // namespace
v1::TableSeriesGroup table_series(const TableStore& store, const v1::TableSeriesRequest& request) {
  protocol::validate_table_series_request(request);
  const auto until = held(store, request.snapshot());
  const auto& table = history_table(daily_table);
  const auto daily = series_of(daily_table, request.daily_source());
  const auto calendar = series_of(calendar_table, request.calendar_source());
  const std::vector<std::size_t> columns{
      table_column(table, "contract"), table_column(table, "close"),
      table_column(table, "settlement"), table_column(table, "open_interest")};
  // Each exchange's days are read once, for all of its instruments.
  std::vector<protocol::TableInstrument> instruments;
  std::map<std::string, std::vector<int>> exchanges;
  for (int i = 0; i < request.instruments_size(); ++i) {
    instruments.push_back(protocol::table_instrument(request.instruments(i)));
    exchanges[instruments.back().venue].push_back(i);
  }
  v1::TableSeriesGroup group;
  for (int i = 0; i < request.instruments_size(); ++i)
    group.add_series();
  std::size_t total = 0;
  for (const auto& [venue, wanted] : exchanges) {
    const auto revisions =
        store.revisions(daily, venue, request.begin_day(), request.end_day(), until);
    std::vector<std::string> missing;
    for (const auto& day :
         open_days(store, calendar, venue, request.begin_day(), request.end_day(), until))
      if (!std::ranges::binary_search(revisions, day, {}, &TableRevision::period))
        missing.push_back(day);
    if (!missing.empty()) {
      constexpr std::size_t shown = 5;
      auto text = "futures.daily is missing trading days of the range: " + venue;
      for (const auto& day : missing | std::views::take(shown))
        text += " " + day;
      if (missing.size() > shown)
        text += " and " + std::to_string(missing.size() - shown) + " more";
      throw Error(ErrorCode::conflict, text);
    }
    std::vector<std::string> prefixes;
    for (const auto index : wanted)
      prefixes.push_back(
          request.instruments(index) +
          (instruments[static_cast<std::size_t>(index)].delivery_month.empty() ? "/" : ""));
    std::map<std::string, Month> months; // by contract
    std::int64_t acquired = 0;
    for (const auto& revision : revisions)
      acquired = std::max(acquired, revision.acquired_at_ns);
    store.scan(revisions, columns, prefixes, [&](std::size_t revision, auto& cells) {
      auto& month = months[std::get<std::string>(cells[0])];
      const auto& day = revisions[revision].period;
      const auto keep = [&](std::map<std::string, Decimal>& values, const TableCell& cell) {
        if (const auto* units = std::get_if<std::int64_t>(&cell))
          values.emplace(day, Decimal::from_raw(*units));
      };
      keep(month.close, cells[1]);
      keep(month.settlement, cells[2]);
      // Kept in lots; only compared, so the scale does not matter.
      keep(month.open_interest, cells[3]);
    });
    for (const auto index : wanted) {
      const auto& name = request.instruments(index);
      const auto& instrument = instruments[static_cast<std::size_t>(index)];
      auto& series = *group.mutable_series(index);
      series.set_version(1);
      series.set_snapshot(request.snapshot());
      series.set_daily_source(request.daily_source());
      series.set_calendar_source(request.calendar_source());
      series.set_begin_day(request.begin_day());
      series.set_end_day(request.end_day());
      series.set_instrument(name);
      series.set_acquired_at_ns(acquired);
      const auto add = [&](v1::TableMonth& target, const Month& month, const std::string& first,
                           const std::string* before) {
        for (auto close = month.close.lower_bound(first);
             close != month.close.end() && (!before || close->first < *before); ++close) {
          auto* row = target.add_closes();
          row->set_trading_day(close->first);
          row->mutable_close()->set_units(close->second.raw());
          ++total;
        }
      };
      if (!instrument.delivery_month.empty()) {
        const auto found = months.find(name);
        if (found == months.end() || found->second.close.empty())
          throw std::invalid_argument("the instrument has no closes in the range: " + name);
        auto* month = series.add_months();
        month->set_contract(name);
        add(*month, found->second, request.begin_day(), nullptr);
        continue;
      }
      // The product's months in delivery order: keys of one product sort so.
      const auto prefix = name + "/";
      std::vector<DominantMonth> facts;
      std::vector<const Month*> rows;
      for (auto month = months.lower_bound(prefix);
           month != months.end() && month->first.starts_with(prefix); ++month) {
        facts.push_back({month->first, month->first.substr(prefix.size()), month->second.settlement,
                         month->second.open_interest});
        rows.push_back(&month->second);
      }
      if (facts.empty())
        throw std::invalid_argument("the instrument has no closes in the range: " + name);
      DominantPlan plan;
      try {
        plan = plan_dominant(facts, RollDay::may_have_expired);
      } catch (const std::invalid_argument& error) {
        throw std::invalid_argument("the dominant series cannot be read: " + name + ": " +
                                    error.what());
      }
      for (std::size_t i = 0; i < plan.segments.size(); ++i) {
        const auto& segment = plan.segments[i];
        auto* month = series.add_months();
        month->set_contract(facts[segment.month].name);
        add(*month, *rows[segment.month], segment.begin,
            i + 1 < plan.segments.size() ? &plan.segments[i + 1].begin : nullptr);
        auto* roll = series.mutable_schedule()->add_rolls();
        roll->set_trading_day(segment.begin);
        roll->set_contract(static_cast<unsigned>(i));
        roll->mutable_factor()->set_units(segment.factor.raw());
      }
      for (auto& term : plan.terms)
        *series.mutable_schedule()->add_terms() = std::move(term);
    }
    if (total > protocol::max_dataset_bars)
      throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
  }
  for (const auto& series : group.series())
    protocol::validate_table_series(series);
  return group;
}
v1::TableProducts table_products(const TableStore& store, const v1::TableProductsQuery& query) {
  const auto until = held(store, query.snapshot());
  const auto& table = history_table(contracts_table);
  const auto contracts = series_of(contracts_table, query.contracts_source());
  const auto contract = table_column(table, "contract"), listed = table_column(table, "list_date"),
             delisted = table_column(table, "delist_date");
  v1::TableProducts result;
  for (const auto venue : history_venues()) {
    const auto revisions = store.revisions(contracts, std::string(venue), "all", "all", until);
    if (revisions.empty())
      continue;
    struct Product {
      unsigned contracts = 0;
      std::optional<std::int64_t> first, last;
    };
    std::map<std::string, Product> products;
    for (const auto& row : store.rows(revisions.front())) {
      auto& product =
          products[HistoryIdentity::parse(std::get<std::string>(row[contract])).product];
      ++product.contracts;
      if (const auto* day = std::get_if<std::int64_t>(&row[listed]);
          day && (!product.first || *day < *product.first))
        product.first = *day;
      if (const auto* day = std::get_if<std::int64_t>(&row[delisted]);
          day && (!product.last || *day > *product.last))
        product.last = *day;
    }
    for (const auto& [name, product] : products) {
      // A product whose contracts carry no dates cannot be placed in time.
      if (!product.first || !product.last)
        continue;
      auto* row = result.add_products();
      row->set_exchange(std::string(venue));
      row->set_product(name);
      row->set_contracts(product.contracts);
      row->set_first_listed(date_text(*product.first));
      row->set_last_delisted(date_text(*product.last));
    }
  }
  return result;
}
} // namespace asterion::data
