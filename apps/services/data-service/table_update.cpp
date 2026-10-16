#include "table_update.hpp"
#include "margins.hpp"
#include "history_providers.hpp"
#include "response_evidence.hpp"
#include <asterion/domain/trading_days.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace asterion::data {
namespace {

using history_files::TableRevision;
using history_files::TableSeries;
using history_files::TableStore;

const TableOffer&
offer(const std::vector<TableOffer>& offers, std::string_view table, const std::string& venue)
{
    const auto found = std::ranges::find(offers, table, [](const auto& item) -> std::string_view {
        return item.series.table;
    });
    if (found == offers.end() || std::ranges::find(found->venues, venue) == found->venues.end())
        throw std::invalid_argument(
            "the source does not provide " + std::string(table) + " for " + venue);
    return *found;
}

const TableOffer& refresh_offer(
    const std::vector<TableOffer>& offers,
    const std::string& provider,
    const std::string& source,
    const v1::TablePartitionKey& key)
{
    const auto found = std::ranges::find_if(offers, [&](const auto& item) {
        return item.plugin_id == provider && item.series.source == source
            && item.series.table == key.table()
            && std::ranges::find(item.venues, key.exchange()) != item.venues.end();
    });
    if (found == offers.end())
        throw Error(
            ErrorCode::conflict,
            "table refresh source is not available from the selected provider");
    return *found;
}

std::string shifted(const std::string& day, int offset)
{
    using namespace std::chrono;
    return format_trading_date(year_month_day(sys_days(parse_trading_date(day)) + days(offset)));
}

std::string date_text(std::int64_t day)
{
    using namespace std::chrono;
    return format_trading_date(year_month_day(sys_days(days(day))));
}

// A partition the day phase cannot do without.
TableRevision reference(
    const TableStore& store,
    const TableSeries& series,
    const std::string& venue,
    const std::string& period)
{
    auto revision = store.current(series, venue, period);
    if (!revision)
        throw Error(
            ErrorCode::conflict,
            "reference data is not stored: " + series.table + " " + venue + " " + period);
    return std::move(*revision);
}

// The first day anything was listed on the exchange.
std::string
inception(const TableStore& store, const TableSeries& contracts, const std::string& venue)
{
    const auto listed = table_column(history_table(contracts_table), "list_date");
    std::optional<std::int64_t> first;
    for (const auto& row : store.rows(reference(store, contracts, venue, "all")))
        if (const auto* day = std::get_if<std::int64_t>(&row[listed]);
            day && (!first || *day < *first))
            first = *day;
    if (!first)
        throw Error(ErrorCode::conflict, "the contract list has no listing date: " + venue);
    return date_text(*first);
}

// Open days of one exchange within [first, last], or nothing when a year of
// the range is not stored.
std::optional<std::vector<std::string>> open_days(
    const TableStore& store,
    const TableSeries& calendar,
    const std::string& venue,
    const std::string& first,
    const std::string& last)
{
    const auto& table = history_table(calendar_table);
    const auto day = table_column(table, "day"), open = table_column(table, "open");
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

void add(
    v1::TableUpdatePlan& plan,
    std::string_view table,
    const std::string& venue,
    const std::string& period)
{
    auto* partition = plan.add_partitions();
    partition->set_table(std::string(table));
    partition->set_exchange(venue);
    partition->set_period(period);
}

} // namespace

v1::TableRefreshPlan table_refresh_plan(
    const TableStore& store,
    const std::vector<TableOffer>& offers,
    const v1::TableRefreshQuery& query,
    const std::string& artifact,
    const std::string& today)
{
    (void)protocol::decode_table_refresh_query(query);
    (void)parse_trading_date(today);
    if (query.partition().period() >= today)
        throw std::invalid_argument("table refresh requires a completed trading date");
    const auto& selected =
        refresh_offer(offers, query.provider(), query.source(), query.partition());
    const auto previous =
        reference(store, selected.series, query.partition().exchange(), query.partition().period());
    if (previous.snapshot != query.previous_snapshot())
        throw Error(
            ErrorCode::conflict,
            "selected table partition changed; read it and preview again");
    v1::TableRefreshPlan plan;
    *plan.mutable_query() = query;
    *plan.mutable_previous() = encode_table_revision(previous);
    auto* update = plan.mutable_update();
    update->set_provider(query.provider());
    update->add_tables(query.partition().table());
    update->set_begin_day(query.partition().period());
    update->set_end_day(query.partition().period());
    update->set_requests_per_minute(query.requests_per_minute());
    auto* target = update->mutable_refresh();
    *target->mutable_partition() = query.partition();
    target->set_source(query.source());
    target->set_previous_snapshot(previous.snapshot);
    target->set_previous_content(previous.content);
    target->set_provider_artifact(artifact);
    protocol::validate_table_update(*update);
    plan.set_id(sha256_bytes(plan.SerializeAsString()));
    return plan;
}

bool table_update_covers(
    const std::vector<TableOffer>& offers,
    const v1::TableUpdate& update,
    const v1::TablePartitionKey& partition)
{
    if (update.has_refresh())
        return partition.SerializeAsString() == update.refresh().partition().SerializeAsString()
            && std::ranges::any_of(offers, [&](const auto& item) {
                   return item.plugin_id == update.provider()
                       && item.series.table == partition.table()
                       && item.series.source == update.refresh().source()
                       && std::ranges::find(item.venues, partition.exchange()) != item.venues.end();
               });
    const auto& table = partition.table();
    if (table != contracts_table && table != calendar_table
        && std::ranges::find(update.tables(), table) == update.tables().end())
        return false;
    return std::ranges::any_of(offers, [&](const TableOffer& item) {
        return item.series.table == table
            && std::ranges::find(item.venues, partition.exchange()) != item.venues.end();
    });
}

v1::TableUpdatePlan table_update_plan(
    const TableStore& store,
    const std::vector<TableOffer>& offers,
    const v1::TableUpdate& update,
    v1::TablePlanStep step,
    const std::string& today)
{
    protocol::validate_table_update(update);
    if (step != v1::TABLE_PLAN_CONTRACTS && step != v1::TABLE_PLAN_CALENDAR
        && step != v1::TABLE_PLAN_DAYS)
        throw std::invalid_argument("invalid table plan step");
    (void)parse_trading_date(today);
    const auto ended = shifted(today, -1);
    const auto end = update.end_day().empty() ? ended : std::min(update.end_day(), ended);
    v1::TableUpdatePlan plan;
    if (update.has_refresh()) {
        const auto& target = update.refresh();
        (void)refresh_offer(offers, update.provider(), target.source(), target.partition());
        if (target.partition().period() >= today)
            throw std::invalid_argument("table refresh requires a completed trading date");
        if (step == v1::TABLE_PLAN_DAYS)
            *plan.add_partitions() = target.partition();
        return plan;
    }

    if (update.provider() == shfe::provider) {
        // Only the explicitly reviewed report dates; this provider has no calendar.
        shfe::validate_scope("SHFE", update.begin_day());
        shfe::validate_scope("SHFE", update.end_day());
        if (update.tables_size() != 1 || update.tables(0) != shfe::table)
            throw std::invalid_argument("history table is unavailable from this source");
        if (step == v1::TABLE_PLAN_DAYS)
            for (const auto* day : shfe::days)
                if (day >= update.begin_day() && day <= end
                    && !store.current({shfe::table, 1, shfe::source}, "SHFE", day))
                    add(plan, shfe::table, "SHFE", day);
        return plan;
    }

    // Where each exchange of each table starts, and which exchanges take part.
    struct Start
    {
        std::string table, venue, first;
    };

    std::vector<Start> starts;
    for (const auto& table : update.tables()) {
        const auto found =
            std::ranges::find(offers, table, [](const auto& item) -> const std::string& {
                return item.series.table;
            });
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
    if (step == v1::TABLE_PLAN_CONTRACTS) {
        std::set<std::string> venues;
        for (const auto& start : starts)
            venues.insert(start.venue);
        for (const auto& venue : venues) {
            (void)offer(offers, contracts_table, venue);
            // New contracts are listed every day: the list is always read again.
            add(plan, contracts_table, venue, "all");
        }
        return plan;
    }
    // Nothing is asked about an exchange before it listed its first contract:
    // a source has neither a calendar nor days for those years.
    std::map<std::string, std::string> inceptions; // by exchange
    for (auto& start : starts) {
        const auto [listed, fresh] = inceptions.try_emplace(start.venue);
        if (fresh)
            listed->second =
                inception(store, offer(offers, contracts_table, start.venue).series, start.venue);
        start.first = std::max(start.first, listed->second);
    }
    std::erase_if(starts, [&](const Start& start) {
        return start.first > end;
    });
    if (step == v1::TABLE_PLAN_CALENDAR) {
        std::map<std::string, std::string> earliest; // by exchange
        for (const auto& start : starts) {
            const auto [found, inserted] = earliest.emplace(start.venue, start.first);
            if (!inserted && start.first < found->second)
                found->second = start.first;
        }
        for (const auto& [venue, first] : earliest) {
            const auto& calendar = offer(offers, calendar_table, venue);
            // A year that has not ended may still change; a finished one is read once.
            for (auto year = std::stoi(first.substr(0, 4)); year <= std::stoi(end.substr(0, 4));
                ++year)
                if (std::to_string(year) >= today.substr(0, 4)
                    || !store.current(calendar.series, venue, std::to_string(year)))
                    add(plan, calendar_table, venue, std::to_string(year));
        }
        return plan;
    }
    std::vector<v1::TablePartitionKey> days;
    for (const auto& start : starts) {
        const auto& series = offer(offers, start.table, start.venue).series;
        const auto& calendar = offer(offers, calendar_table, start.venue).series;
        const auto open = open_days(store, calendar, start.venue, start.first, end);
        if (!open)
            throw Error(
                ErrorCode::conflict,
                "reference data is not stored: " + std::string(calendar_table) + " " + start.venue
                    + " for a year of the range");
        const auto held = store.periods(series, start.venue, start.first, end);
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

v1::TableRevision encode_table_revision(const TableRevision& revision)
{
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

v1::TableCoverage table_coverage(const TableStore& store, const std::vector<TableOffer>& installed)
{
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
        const auto known = std::ranges::find(
            history_tables(),
            std::string_view(held.series.table),
            &HistoryTable::id);
        if (provided == installed.end() || known == history_tables().end()
            || known->schema != held.series.schema || known->period != TablePeriod::day)
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

v1::TableRefreshObservation encode_refresh_observation(
    const history_files::TableRefreshObservation& o)
{
    history_providers::validate_response(
        o.revision.series.table,
        o.revision.exchange,
        o.revision.period,
        o.response);
    v1::TableRefreshObservation value;
    value.set_id(o.id);
    value.set_operation(o.operation);
    value.set_data_instance(o.context.data_instance);
    value.set_task_instance(o.context.task_instance);
    value.set_task_id(o.context.task_id);
    value.set_attempt(o.context.attempt);
    value.set_provider(o.context.provider);
    value.set_provider_artifact(o.context.provider_artifact);
    value.set_selected_snapshot(o.context.selected_snapshot);
    value.set_selected_content(o.context.selected_content);
    *value.mutable_revision() = encode_table_revision(o.revision);
    auto* raw = value.mutable_response();
    raw->set_format(o.response.format);
    raw->set_body(o.response.body);
    raw->set_received_at_ns(o.response.received_at_ns);
    value.set_response_sha256(sha256_bytes(o.response.body));
    value.set_response_bytes(o.response.body.size());
    value.set_recorded_at_ns(o.recorded_at_ns);
    value.set_unchanged(o.unchanged);
    return value;
}

v1::TablePage table_page(const TableStore& store, const v1::TablePageQuery& query)
{
    protocol::validate_message(query);
    const auto& table = history_table(query.partition().table());
    if (query.limit() < 1 || query.limit() > 500)
        throw std::invalid_argument("invalid history table page query");
    const auto revision = store.current(
        {query.partition().table(), table.schema, query.source()},
        query.partition().exchange(),
        query.partition().period());
    if (!revision)
        throw Error(ErrorCode::not_found, "history table partition is not stored");
    const TablePartition partition{
        &table,
        revision->exchange,
        revision->period,
        store.rows(*revision),
        revision->content
    };
    const auto& rows = partition.rows;
    if (query.offset() > rows.size() || (query.offset() == rows.size() && !rows.empty()))
        throw std::invalid_argument("history table page offset is out of range");
    v1::TablePage page;
    *page.mutable_revision() = encode_table_revision(*revision);
    const auto response = store.response(*revision);
    if (response) {
        history_providers::validate_response(
            revision->series.table,
            revision->exchange,
            revision->period,
            *response);
        auto* evidence = page.mutable_response_evidence();
        auto* raw = evidence->mutable_response();
        raw->set_format(response->format);
        raw->set_received_at_ns(response->received_at_ns);
        raw->set_body(response->body);
        evidence->set_sha256(sha256_bytes(response->body));
        evidence->set_bytes(response->body.size());
        evidence->set_revision_snapshot(revision->snapshot);
        evidence->set_normalized_content(revision->content);
        evidence->set_normalized_object(revision->object);
        evidence->set_provider_artifact(revision->provider_artifact);
    }
    const auto review = tushare::review_daily_response(response, partition);
    auto* checked = page.mutable_response_review();
    checked->set_status(review.status);
    checked->set_reason(review.reason);
    checked->set_response_fields(review.response_fields);
    checked->set_response_rows(review.response_rows);
    checked->set_normalized_rows(review.normalized_rows);
    checked->set_dated_rows(review.dated_rows);
    checked->set_matched_rows(review.matched_rows);
    checked->set_compared_cells(review.compared_cells);
    for (const auto& key : review.skipped_keys)
        checked->add_skipped_keys(key);
    for (const auto& key : review.unknown_keys)
        checked->add_unknown_keys(key);
    for (const auto& issue : review.differences) {
        auto* difference = checked->add_differences();
        difference->set_kind(issue.kind);
        difference->set_key(issue.key);
        difference->set_field(issue.field);
        if (issue.raw)
            difference->set_raw(*issue.raw);
        if (issue.expected)
            difference->set_expected(*issue.expected);
        if (issue.actual)
            difference->set_actual(*issue.actual);
    }
    if (const auto observation = store
            .latest_refresh_observation(revision->series, revision->exchange, revision->period))
        *page.mutable_latest_refresh_observation() = encode_refresh_observation(*observation);
    page.set_offset(query.offset());
    for (const auto& column : table.columns)
        page.add_columns(std::string(column.name));
    for (auto r = query.offset();
        r < rows.size() && page.rows_size() < static_cast<int>(query.limit());
        ++r) {
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
