#include "table_update.hpp"
#include <asterion/domain/daily_bars.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <tuple>
namespace asterion::data {
namespace {
using history_files::TableRevision;
using history_files::TableSeries;
using history_files::TableStore;
const TableOffer& offer(const std::vector<TableOffer>& offers, std::string_view table,
                        const std::string& venue) {
  const auto found = std::ranges::find(
      offers, table, [](const auto& item) -> std::string_view { return item.series.table; });
  if (found == offers.end() || std::ranges::find(found->venues, venue) == found->venues.end())
    throw std::invalid_argument("the source does not provide " + std::string(table) + " for " +
                                venue);
  return *found;
}
std::string shifted(const std::string& day, int offset) {
  using namespace std::chrono;
  return format_trading_date(year_month_day(sys_days(parse_trading_date(day)) + days(offset)));
}
std::size_t column(const HistoryTable& table, std::string_view name) {
  const auto found = std::ranges::find(table.columns, name, &TableColumn::name);
  if (found == table.columns.end())
    throw std::logic_error("history table has no such column");
  return static_cast<std::size_t>(found - table.columns.begin());
}
std::string date_text(std::int64_t day) {
  using namespace std::chrono;
  return format_trading_date(year_month_day(sys_days(days(day))));
}
// A partition the day phase cannot do without.
TableRevision reference(const TableStore& store, const TableSeries& series,
                        const std::string& venue, const std::string& period) {
  auto revision = store.current(series, venue, period);
  if (!revision)
    throw Error(ErrorCode::conflict,
                "reference data is not stored: " + series.table + " " + venue + " " + period);
  return std::move(*revision);
}
// The first day anything was listed on the exchange.
std::string inception(const TableStore& store, const TableSeries& contracts,
                      const std::string& venue) {
  const auto listed = column(history_table(contracts_table), "list_date");
  std::optional<std::int64_t> first;
  for (const auto& row : store.rows(reference(store, contracts, venue, "all")))
    if (const auto* day = std::get_if<std::int64_t>(&row[listed]); day && (!first || *day < *first))
      first = *day;
  if (!first)
    throw Error(ErrorCode::conflict, "the contract list has no listing date: " + venue);
  return date_text(*first);
}
// Open days of one exchange within [first, last], or nothing when a year of
// the range is not stored.
std::optional<std::vector<std::string>>
open_days(const TableStore& store, const TableSeries& calendar, const std::string& venue,
          const std::string& first, const std::string& last) {
  const auto& table = history_table(calendar_table);
  const auto day = column(table, "day"), open = column(table, "open");
  std::vector<std::string> result;
  for (auto year = std::stoi(first.substr(0, 4)); year <= std::stoi(last.substr(0, 4)); ++year) {
    const auto revision = store.current(calendar, venue, std::to_string(year));
    if (!revision)
      return std::nullopt;
    for (const auto& row : store.rows(*revision)) {
      const auto* flag = std::get_if<std::int64_t>(&row[open]);
      const auto text = date_text(std::get<std::int64_t>(row[day]));
      if (flag && *flag && text >= first && text <= last)
        result.push_back(text);
    }
  }
  std::ranges::sort(result);
  return result;
}
void add(v1::TableUpdatePlan& plan, std::string_view table, const std::string& venue,
         const std::string& period) {
  auto* partition = plan.add_partitions();
  partition->set_table(std::string(table));
  partition->set_exchange(venue);
  partition->set_period(period);
}
} // namespace
bool table_update_covers(const std::vector<TableOffer>& offers, const v1::TableUpdate& update,
                         const v1::TablePartitionKey& partition) {
  const auto& table = partition.table();
  if (table != contracts_table && table != calendar_table &&
      std::ranges::find(update.tables(), table) == update.tables().end())
    return false;
  return std::ranges::any_of(offers, [&](const TableOffer& item) {
    return item.series.table == table &&
           std::ranges::find(item.venues, partition.exchange()) != item.venues.end();
  });
}
v1::TableUpdatePlan table_update_plan(const TableStore& store,
                                      const std::vector<TableOffer>& offers,
                                      const v1::TableUpdate& update, bool references,
                                      const std::string& today) {
  protocol::validate_table_update(update);
  (void)parse_trading_date(today);
  const auto ended = shifted(today, -1);
  const auto end = update.end_day().empty() ? ended : std::min(update.end_day(), ended);
  v1::TableUpdatePlan plan;
  // Where each exchange of each table starts, and which exchanges take part.
  struct Start {
    std::string table, venue, first;
  };
  std::vector<Start> starts;
  for (const auto& table : update.tables()) {
    const auto found = std::ranges::find(
        offers, table, [](const auto& item) -> const std::string& { return item.series.table; });
    if (found == offers.end())
      throw std::invalid_argument("the source does not provide " + table);
    for (const auto& venue : found->venues) {
      auto first = update.begin_day();
      if (first.empty()) {
        const auto held = store.periods(found->series, venue, "0000-00-00", "9999-99-99");
        if (held.empty()) {
          plan.add_waiting(table + " " + venue);
          continue;
        }
        first = shifted(held.back(), 1);
      }
      starts.push_back({table, venue, std::move(first)});
    }
  }
  if (references) {
    std::map<std::string, std::string> earliest; // by exchange
    for (const auto& start : starts) {
      const auto [found, inserted] = earliest.emplace(start.venue, start.first);
      if (!inserted && start.first < found->second)
        found->second = start.first;
    }
    for (const auto& [venue, first] : earliest) {
      (void)offer(offers, contracts_table, venue);
      // New contracts are listed every day: the list is always read again.
      add(plan, contracts_table, venue, "all");
    }
    for (const auto& [venue, first] : earliest) {
      const auto& calendar = offer(offers, calendar_table, venue);
      // A year that has not ended may still change; a finished one is read once.
      for (auto year = std::stoi(first.substr(0, 4)); year <= std::stoi(end.substr(0, 4)); ++year)
        if (std::to_string(year) >= today.substr(0, 4) ||
            !store.current(calendar.series, venue, std::to_string(year)))
          add(plan, calendar_table, venue, std::to_string(year));
    }
    return plan;
  }
  std::map<std::string, std::string> inceptions;
  std::vector<v1::TablePartitionKey> days;
  for (const auto& start : starts) {
    const auto& series = offer(offers, start.table, start.venue).series;
    auto listed = inceptions.find(start.venue);
    if (listed == inceptions.end())
      listed = inceptions
                   .emplace(start.venue,
                            inception(store, offer(offers, contracts_table, start.venue).series,
                                      start.venue))
                   .first;
    const auto first = std::max(start.first, listed->second);
    if (first > end)
      continue;
    const auto& calendar = offer(offers, calendar_table, start.venue).series;
    const auto open = open_days(store, calendar, start.venue, first, end);
    if (!open)
      throw Error(ErrorCode::conflict,
                  "reference data is not stored: " + std::string(calendar_table) + " " +
                      start.venue + " for a year of the range");
    const auto held = store.periods(series, start.venue, first, end);
    for (const auto& day : *open)
      if (!std::ranges::binary_search(held, day)) {
        auto& partition = days.emplace_back();
        partition.set_table(start.table);
        partition.set_exchange(start.venue);
        partition.set_period(day);
      }
  }
  // Oldest day first, so an interrupted run leaves no gap behind what it stored.
  std::ranges::sort(days, {}, [](const auto& partition) {
    return std::tuple(partition.period(), partition.table(), partition.exchange());
  });
  for (auto& partition : days)
    *plan.add_partitions() = std::move(partition);
  return plan;
}
v1::TableRevision encode_table_revision(const TableRevision& revision) {
  v1::TableRevision result;
  result.set_sequence(revision.sequence);
  result.set_table(revision.series.table);
  result.set_schema(revision.series.schema);
  result.set_source(revision.series.source);
  result.set_exchange(revision.exchange);
  result.set_period(revision.period);
  result.set_content(revision.content);
  result.set_object(revision.object);
  result.set_rows(revision.rows);
  result.set_bytes(revision.bytes);
  result.set_acquired_at_ns(revision.acquired_at_ns);
  result.set_provider_artifact(revision.provider_artifact);
  result.set_snapshot(revision.snapshot);
  return result;
}
v1::TableCoverage table_coverage(const TableStore& store,
                                 const std::vector<TableOffer>& installed) {
  v1::TableCoverage result;
  result.set_snapshot(store.head());
  for (const auto& item : installed) {
    auto* source = result.add_sources();
    source->set_table(item.series.table);
    source->set_schema(item.series.schema);
    source->set_source(item.series.source);
    source->set_plugin_id(item.plugin_id);
    for (const auto& venue : item.venues)
      source->add_venues(venue);
  }
  for (const auto& held : store.coverage()) {
    auto* row = result.add_rows();
    row->set_table(held.series.table);
    row->set_schema(held.series.schema);
    row->set_source(held.series.source);
    row->set_exchange(held.exchange);
    row->set_partitions(held.partitions);
    row->set_rows(held.rows);
    row->set_first_period(held.first_period);
    row->set_last_period(held.last_period);
    row->set_acquired_at_ns(held.acquired_at_ns);
    // Gaps are judged against the calendar of the plugin that provides the
    // series. Without it, or for a table that is not kept by day, there is
    // nothing to judge against and no count is given.
    const auto provided = std::ranges::find(installed, held.series, &TableOffer::series);
    const auto known =
        std::ranges::find(history_tables(), std::string_view(held.series.table), &HistoryTable::id);
    if (provided == installed.end() || known == history_tables().end() ||
        known->schema != held.series.schema || known->period != TablePeriod::day)
      continue;
    const auto calendar = std::ranges::find_if(installed, [&](const TableOffer& item) {
      return item.plugin_id == provided->plugin_id && item.series.table == calendar_table;
    });
    if (calendar == installed.end())
      continue;
    const auto open =
        open_days(store, calendar->series, held.exchange, held.first_period, held.last_period);
    if (!open)
      continue;
    const auto stored =
        store.periods(held.series, held.exchange, held.first_period, held.last_period);
    unsigned missing = 0;
    for (const auto& day : *open)
      if (!std::ranges::binary_search(stored, day)) {
        if (++missing <= 10)
          row->add_missing_examples(day);
      }
    row->set_missing_days(missing);
  }
  return result;
}
v1::TablePage table_page(const TableStore& store, const v1::TablePageQuery& query) {
  protocol::validate_message(query);
  const auto& table = history_table(query.partition().table());
  if (query.limit() < 1 || query.limit() > 500)
    throw std::invalid_argument("invalid history table page query");
  const auto revision = store.current({query.partition().table(), table.schema, query.source()},
                                      query.partition().exchange(), query.partition().period());
  if (!revision)
    throw Error(ErrorCode::not_found, "history table partition is not stored");
  const auto rows = store.rows(*revision);
  if (query.offset() > rows.size() || (query.offset() == rows.size() && !rows.empty()))
    throw std::invalid_argument("history table page offset is out of range");
  v1::TablePage page;
  *page.mutable_revision() = encode_table_revision(*revision);
  page.set_offset(query.offset());
  for (const auto& column : table.columns)
    page.add_columns(std::string(column.name));
  for (auto r = query.offset();
       r < rows.size() && page.rows_size() < static_cast<int>(query.limit()); ++r) {
    auto* row = page.add_rows();
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
      auto* cell = row->add_cells();
      if (auto text = table_cell_text(table.columns[c], rows[r][c]))
        cell->set_text(std::move(*text));
    }
  }
  return page;
}
} // namespace asterion::data
