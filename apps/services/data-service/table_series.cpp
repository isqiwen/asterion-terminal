#include "table_series.hpp"
#include "calendar_bars.hpp"
#include "dominant_plan.hpp"
#include "macd.hpp"
#include "table_update.hpp"
#include <asterion/domain/trading_days.hpp>
#include <asterion/domain/history_identity.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <chrono>
#include <numeric>
#include <optional>
#include <set>

namespace asterion::data {
namespace {

using history_files::TableRevision;
using history_files::TableSeries;
using history_files::TableStore;
constexpr std::string_view daily_table = "futures.daily";

std::string date_text(std::int64_t day)
{
    using namespace std::chrono;
    return format_trading_date(year_month_day(sys_days(days(day))));
}

TableSeries series_of(std::string_view table, const std::string& source)
{
    return {std::string(table), history_table(table).schema, source};
}

std::uint64_t held(const TableStore& store, const std::string& snapshot)
{
    const auto until = store.sequence(snapshot);
    if (!until)
        throw Error(ErrorCode::not_found, "the table snapshot is not in this store");
    return *until;
}

// Days of [first, last] the snapshot's calendar has open.
std::vector<std::string> open_days(
    const TableStore& store,
    const TableSeries& calendar,
    const std::string& venue,
    const std::string& first,
    const std::string& last,
    std::uint64_t until)
{
    const auto& table = history_table(calendar_table);
    const auto day = table_column(table, "day"), open = table_column(table, "open");
    std::vector<std::string> result;
    for (auto year = std::stoi(first.substr(0, 4)); year <= std::stoi(last.substr(0, 4)); ++year) {
        const auto text = std::to_string(year);
        const auto revisions = store.revisions(calendar, venue, text, text, until);
        if (revisions.empty())
            throw Error(
                ErrorCode::conflict,
                "the calendar of the range is not stored: " + std::string(calendar_table) + " "
                    + venue + " " + text);
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
struct Month
{
    std::map<std::string, Decimal> close, settlement, open_interest;
    std::map<std::string, std::int64_t> volume_contracts, interest_contracts;
    std::map<std::string, long double> lot_value; // where they were read
    std::map<std::string, DailyTrade> trades; // where they were read
};
// How much of a contract's day is read: what it settled at and held; also
// what a lot of it was worth; also the prices it traded at.
enum class Read
{
    settled,
    valued,
    traded
};

struct Days
{
    std::map<std::string, Month> months; // by contract
    std::map<std::string, std::int64_t> acquired; // by trading day
};

// The contracts of one exchange whose names begin with one of `prefixes`, on
// the days of [begin, end]. Every day the snapshot's calendar has open in the
// range must be stored: a missing one is named, not read around.
Days read_days(
    const TableStore& store,
    const TableSeries& daily,
    const TableSeries& calendar,
    const std::string& venue,
    const std::string& begin,
    const std::string& end,
    std::uint64_t until,
    const std::vector<std::string>& prefixes,
    Read read)
{
    const auto& table = history_table(daily_table);
    std::vector<std::size_t> columns{
        table_column(table, "contract"),
        table_column(table, "close"),
        table_column(table, "settlement"),
        table_column(table, "open_interest"),
        table_column(table, "volume")
    };
    if (read != Read::settled)
        for (const auto* name : {"amount"})
            columns.push_back(table_column(table, name));
    // The other prices are read only for bars.
    if (read == Read::traded)
        for (const auto* name : {"open", "high", "low"})
            columns.push_back(table_column(table, name));
    const auto revisions = store.revisions(daily, venue, begin, end, until);
    std::vector<std::string> missing;
    for (const auto& day : open_days(store, calendar, venue, begin, end, until))
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
    Days result;
    for (const auto& revision : revisions)
        result.acquired.emplace(revision.period, revision.acquired_at_ns);
    store.scan(revisions, columns, prefixes, [&](std::size_t revision, auto& cells) {
        auto& month = result.months[std::get<std::string>(cells[0])];
        const auto& day = revisions[revision].period;
        const auto keep = [&](std::map<std::string, Decimal>& values, const TableCell& cell) {
            if (const auto* units = std::get_if<std::int64_t>(&cell))
                values.emplace(day, Decimal::from_raw(*units));
        };
        keep(month.close, cells[1]);
        keep(month.settlement, cells[2]);
        // Kept in lots; only compared, so the scale does not matter.
        keep(month.open_interest, cells[3]);
        if (const auto* value = std::get_if<std::int64_t>(&cells[3]))
            month.interest_contracts.emplace(day, *value);
        if (const auto* value = std::get_if<std::int64_t>(&cells[4]))
            month.volume_contracts.emplace(day, *value);
        if (read == Read::settled)
            return;
        const auto value = [&](std::size_t cell) {
            return std::get_if<std::int64_t>(&cells[cell]);
        };
        const auto *volume = value(4), *amount = value(5);
        // The amount is kept to two places and the volume in whole lots: what a
        // lot was worth, a price times the contract's multiplier.
        if (volume && amount && *volume > 0 && *amount > 0)
            month.lot_value.emplace(day, static_cast<long double>(*amount) / 100 / *volume);
        if (read != Read::traded)
            return;
        const auto *open = value(6), *high = value(7), *low = value(8), *close = value(1);
        // The table has an open, a high and a low only where the contract traded
        // in the auction. A day nobody traded it has none, and neither has a day
        // whose only volume changed hands outside the auction, as in a delivery
        // month: there is no price to fill an order at on either.
        // Prices are kept to eight places, which is Decimal's own scale; the
        // volume is kept in whole lots.
        if (open && high && low && close && volume)
            month.trades.emplace(
                day,
                DailyTrade{
                    Decimal::from_raw(*open),
                    Decimal::from_raw(*high),
                    Decimal::from_raw(*low),
                    Decimal::from_raw(*close),
                    Decimal::parse(std::to_string(*volume))
                });
    });
    return result;
}

} // namespace

historical_costs::MarginFact settlement_margin_fact(
    const TableStore& store,
    const std::string& snapshot,
    const TableSeries& series,
    const std::string& contract,
    const std::string& observed_day)
{
    if (series.table != "futures.settlement" || series.schema != 1 || series.source.empty())
        throw std::invalid_argument("historical margin source evidence is missing");
    (void)parse_trading_date(observed_day);
    const auto identity = HistoryIdentity::parse(contract);
    const auto until = store.sequence(snapshot);
    if (!until)
        throw std::invalid_argument("historical margin snapshot is unavailable");
    const auto revisions =
        store.revisions(series, identity.venue, observed_day, observed_day, *until);
    if (revisions.empty())
        throw std::invalid_argument("historical margin partition is missing");
    const auto& revision = revisions.front();
    const auto& schema = history_table("futures.settlement");
    const auto key = table_column(schema, "contract"),
               long_column = table_column(schema, "long_margin_rate"),
               short_column = table_column(schema, "short_margin_rate");
    for (const auto& row : store.rows(revision)) {
        if (std::get<std::string>(row[key]) != contract)
            continue;
        const auto value = [&](std::size_t column) -> std::optional<Decimal> {
            if (const auto* number = std::get_if<std::int64_t>(&row[column]))
                return Decimal::from_raw(*number);
            return {};
        };
        return {
            contract,
            observed_day,
            series.source,
            snapshot,
            revision.snapshot,
            revision.content,
            revision.object,
            revision.provider_artifact,
            revision.acquired_at_ns,
            value(long_column),
            value(short_column)
        };
    }
    throw std::invalid_argument("historical margin contract row is missing");
}

DailyFacts daily_facts(
    const TableStore& store,
    const v1::DailyTable& table,
    const std::string& venue,
    const std::string& begin_day,
    const std::string& end_day,
    const std::vector<std::string>& names,
    bool trades)
{
    protocol::validate_daily_table(table);
    auto days = read_days(
        store,
        series_of(daily_table, table.daily_source()),
        series_of(calendar_table, table.calendar_source()),
        venue,
        begin_day,
        end_day,
        held(store, table.snapshot()),
        names,
        trades ? Read::traded : Read::valued);
    DailyFacts result;
    result.acquired_at_ns = std::move(days.acquired);
    for (auto& [contract, month] : days.months) {
        // A longer name that begins with a contract's is another contract; one
        // that begins with a product's, slash and all, is a contract of it.
        if (std::ranges::none_of(names, [&](const std::string& name) {
                return name == contract || (name.ends_with('/') && contract.starts_with(name));
            }))
            continue;
        result.settlement.emplace(contract, std::move(month.settlement));
        result.open_interest.emplace(contract, std::move(month.open_interest));
        result.lot_value.emplace(contract, std::move(month.lot_value));
        if (trades)
            result.trades.emplace(contract, std::move(month.trades));
    }
    return result;
}

std::map<std::string, Decimal> price_increments(
    const TableStore& store,
    const v1::DailyTable& table,
    const std::string& venue,
    const std::string& product,
    const std::string& begin_day,
    const std::string& end_day)
{
    protocol::validate_daily_table(table);
    // Three months either side hold the stored days the windows reach.
    const auto shifted = [](const std::string& day, int by) {
        using namespace std::chrono;
        return format_trading_date(year_month_day(sys_days(parse_trading_date(day)) + days(by)));
    };
    const auto stored = store.revisions(
        series_of(daily_table, table.daily_source()),
        venue,
        shifted(begin_day, -92),
        shifted(end_day, 92),
        held(store, table.snapshot()));
    const auto first = std::ranges::lower_bound(stored, begin_day, {}, &TableRevision::period);
    const auto last = std::ranges::upper_bound(stored, end_day, {}, &TableRevision::period);
    const std::vector<TableRevision> read(
        first - std::min<std::ptrdiff_t>(first - stored.begin(), increment_days),
        last + std::min<std::ptrdiff_t>(stored.end() - last, increment_days));
    const auto& definition = history_table(daily_table);
    const std::size_t columns[] = {
        table_column(definition, "contract"),
        table_column(definition, "open"),
        table_column(definition, "high"),
        table_column(definition, "low"),
        table_column(definition, "close")
    };
    const std::string prefixes[] = {venue + "/" + product + "/"};
    // By trading day, the greatest common divisor of the prices it traded at
    // in the auction: a month that did not has no open, high or low.
    std::map<std::string, std::int64_t> divisors;
    store.scan(read, columns, prefixes, [&](std::size_t revision, auto& cells) {
        std::int64_t divisor = 0;
        for (std::size_t c = 1; c < std::size(columns); ++c) {
            const auto* units = std::get_if<std::int64_t>(&cells[c]);
            if (!units)
                return;
            divisor = std::gcd(divisor, *units);
        }
        auto& day = divisors[read[revision].period];
        day = std::gcd(day, divisor);
    });
    const std::vector<std::pair<std::string, std::int64_t>> traded(
        divisors.begin(),
        divisors.end());
    const auto window = std::min(increment_days, traded.size());
    const auto divisor = [&](std::size_t from) {
        std::int64_t result = 0;
        for (auto i = from; i < from + window; ++i)
            result = std::gcd(result, traded[i].second);
        return result;
    };
    std::map<std::string, Decimal> result;
    for (std::size_t i = 0; i < traded.size(); ++i)
        if (traded[i].first >= begin_day && traded[i].first <= end_day)
            result.emplace(
                traded[i].first,
                Decimal::from_raw(
                    std::max(
                        divisor(i + 1 >= window ? i + 1 - window : 0),
                        divisor(std::min(i, traded.size() - window)))));
    return result;
}

namespace {

// A lot in units of what the price is quoted per, by the units the contract
// table names: one where the price is per the unit the contract trades in, or
// per no unit at all (index points, yuan of a bond), the multiplier saying
// the rest; two where it is per 500 kg of a contract traded in tonnes. The
// table's own pairs are these; another is not guessed at.
std::optional<Decimal> quoted_lots(const TableCell& trade_unit, const TableCell& quote_unit)
{
    const auto* traded = std::get_if<std::string>(&trade_unit);
    const auto* quoted = std::get_if<std::string>(&quote_unit);
    if (!traded || !quoted)
        return std::nullopt;
    const auto slash = quoted->find('/');
    if (slash == std::string::npos)
        return Decimal::parse("1");
    const auto per = std::string_view(*quoted).substr(slash + 1);
    if (per == *traded)
        return Decimal::parse("1");
    if (per == "500千克" && *traded == "吨")
        return Decimal::parse("2");
    return std::nullopt;
}

} // namespace

std::map<std::string, Decimal> contract_multipliers(
    const TableStore& store,
    const v1::DailyTable& table,
    const std::string& venue,
    const std::vector<std::string>& contracts)
{
    protocol::validate_daily_table(table);
    const auto revisions = store.revisions(
        series_of(contracts_table, table.contracts_source()),
        venue,
        "all",
        "all",
        held(store, table.snapshot()));
    if (revisions.empty())
        throw Error(ErrorCode::conflict, "the contract table of " + venue + " is not stored");
    const auto& definition = history_table(contracts_table);
    const auto contract = table_column(definition, "contract"),
               multiplier = table_column(definition, "multiplier"),
               per_unit = table_column(definition, "per_unit"),
               trade_unit = table_column(definition, "trade_unit"),
               quote_unit = table_column(definition, "quote_unit");
    const std::set<std::string> wanted(contracts.begin(), contracts.end());
    std::map<std::string, Decimal> result;
    for (const auto& row : store.rows(revisions.front())) {
        const auto& name = std::get<std::string>(row[contract]);
        if (!wanted.contains(name))
            continue;
        const auto* lot = std::get_if<std::int64_t>(&row[per_unit]);
        const auto lots = quoted_lots(row[trade_unit], row[quote_unit]);
        if (!lot || *lot <= 0 || !lots)
            throw std::invalid_argument(
                "the contract table's units of " + name + " cannot be read");
        const auto* scale = std::get_if<std::int64_t>(&row[multiplier]);
        result.emplace(
            name,
            (scale ? Decimal::from_raw(*scale) : Decimal::parse("1")) * Decimal::from_raw(*lot)
                * *lots);
    }
    for (const auto& name : wanted)
        if (!result.contains(name))
            throw std::invalid_argument("the contract table has no " + name);
    return result;
}

std::string previous_trading_day(
    const TableStore& store,
    const v1::DailyTable& table,
    const std::string& venue,
    const std::string& first_day)
{
    protocol::validate_daily_table(table);
    const auto date = parse_trading_date(first_day);
    const auto until = held(store, table.snapshot());
    const auto calendar = series_of(calendar_table, table.calendar_source());
    if (open_days(store, calendar, venue, first_day, first_day, until).empty())
        throw std::invalid_argument("dominant replay anchor is not an open trading day");
    const auto year = static_cast<int>(date.year());
    auto days =
        open_days(store, calendar, venue, std::to_string(year) + "-01-01", first_day, until);
    if (days.size() > 1)
        return days[days.size() - 2];
    days = open_days(
        store,
        calendar,
        venue,
        std::to_string(year - 1) + "-01-01",
        std::to_string(year - 1) + "-12-31",
        until);
    if (days.empty())
        throw std::invalid_argument("dominant replay lacks the previous calendar trading day");
    return days.back();
}

v1::TableSeriesGroup table_series(const TableStore& store, const v1::TableSeriesRequest& request)
{
    protocol::validate_table_series_request(request);
    const auto until = held(store, request.snapshot());
    const auto daily = series_of(daily_table, request.daily_source());
    const auto calendar = series_of(calendar_table, request.calendar_source());
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
    // Instruments that cannot be read, each with why, by their place in the
    // request: all of them are named together, not the first one met.
    std::map<int, std::string> unreadable;
    for (const auto& [venue, wanted] : exchanges) {
        std::vector<std::string> prefixes;
        for (const auto index : wanted)
            prefixes.push_back(
                request.instruments(index)
                + (instruments[static_cast<std::size_t>(index)].delivery_month.empty() ? "/" : ""));
        const auto days = read_days(
            store,
            daily,
            calendar,
            venue,
            request.begin_day(),
            request.end_day(),
            until,
            prefixes,
            Read::settled);
        const auto& months = days.months;
        std::int64_t acquired = 0;
        for (const auto& [day, time] : days.acquired)
            acquired = std::max(acquired, time);
        for (const auto index : wanted) {
            const auto& name = request.instruments(index);
            const auto& instrument = instruments[static_cast<std::size_t>(index)];
            auto& series = *group.mutable_series(index);
            series.set_snapshot(request.snapshot());
            series.set_daily_source(request.daily_source());
            series.set_calendar_source(request.calendar_source());
            series.set_begin_day(request.begin_day());
            series.set_end_day(request.end_day());
            series.set_instrument(name);
            series.set_acquired_at_ns(acquired);
            const auto add = [&](v1::TableMonth& target,
                                 const Month& month,
                                 const std::string& first,
                                 const std::string* before) {
                for (auto close = month.close.lower_bound(first);
                    close != month.close.end() && (!before || close->first < *before);
                    ++close) {
                    auto* row = target.add_closes();
                    row->set_trading_day(close->first);
                    row->mutable_close()->set_units(close->second.raw());
                    if (const auto value = month.volume_contracts.find(close->first);
                        value != month.volume_contracts.end())
                        row->set_volume_contracts(value->second);
                    if (const auto value = month.interest_contracts.find(close->first);
                        value != month.interest_contracts.end())
                        row->set_open_interest_contracts(value->second);
                    ++total;
                }
            };
            if (!instrument.delivery_month.empty()) {
                const auto found = months.find(name);
                if (found == months.end() || found->second.close.empty()) {
                    unreadable.emplace(index, "it has no closes in the range");
                    continue;
                }
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
                month != months.end() && month->first.starts_with(prefix);
                ++month) {
                facts.push_back(
                    {month->first,
                        month->first.substr(prefix.size()),
                        month->second.settlement,
                        month->second.open_interest});
                rows.push_back(&month->second);
            }
            if (facts.empty()) {
                unreadable.emplace(index, "it has no closes in the range");
                continue;
            }
            DominantPlan plan;
            try {
                plan = plan_dominant(facts, SeriesUse::read);
            } catch (const std::invalid_argument& error) {
                unreadable.emplace(index, error.what());
                continue;
            }
            for (std::size_t i = 0; i < plan.segments.size(); ++i) {
                const auto& segment = plan.segments[i];
                auto* month = series.add_months();
                month->set_contract(facts[segment.month].name);
                add(*month,
                    *rows[segment.month],
                    segment.begin,
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
    if (!unreadable.empty()) {
        constexpr std::size_t shown = 20;
        std::string text = "instruments cannot be read: ";
        std::size_t listed = 0;
        for (const auto& [index, why] : unreadable | std::views::take(shown))
            text += (listed++ ? "; " : "") + request.instruments(index) + ": " + why;
        if (unreadable.size() > shown)
            text += " and " + std::to_string(unreadable.size() - shown) + " more";
        throw std::invalid_argument(text);
    }
    for (const auto& series : group.series())
        protocol::validate_table_series(series);
    return group;
}

v1::TableProducts table_products(const TableStore& store, const v1::TableProductsQuery& query)
{
    const auto until = held(store, query.snapshot());
    const auto& table = history_table(contracts_table);
    const auto contracts = series_of(contracts_table, query.contracts_source());
    const auto contract = table_column(table, "contract"),
               listed = table_column(table, "list_date"),
               delisted = table_column(table, "delist_date");
    v1::TableProducts result;
    for (const auto venue : history_venues()) {
        const auto revisions = store.revisions(contracts, std::string(venue), "all", "all", until);
        if (revisions.empty())
            continue;

        struct Product
        {
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

v1::TableListedContracts
table_listed_contracts(const TableStore& store, const v1::TableListedContractsQuery& query)
{
    protocol::validate_table_listed_contracts_query(query);
    const auto lists = store.revisions(
        series_of(contracts_table, query.contracts_source()),
        query.exchange(),
        "all",
        "all",
        held(store, query.snapshot()));
    if (lists.empty())
        throw Error(ErrorCode::conflict, "no contract list of " + query.exchange() + " is stored");
    const auto& table = history_table(contracts_table);
    const auto contract = table_column(table, "contract"), name = table_column(table, "name"),
               listed = table_column(table, "list_date"),
               delisted = table_column(table, "delist_date");
    v1::TableListedContracts result;
    *result.mutable_query() = query;
    for (const auto& row : store.rows(lists.front())) {
        const auto& key = std::get<std::string>(row[contract]);
        const auto* first = std::get_if<std::int64_t>(&row[listed]);
        const auto* last = std::get_if<std::int64_t>(&row[delisted]);
        if (HistoryIdentity::parse(key).product != query.product() || !first || !last)
            continue;
        auto* item = result.add_items();
        item->set_contract_id(key);
        if (const auto* text = std::get_if<std::string>(&row[name]))
            item->set_name(*text);
        item->set_list_date(date_text(*first));
        item->set_delist_date(date_text(*last));
    }
    return result;
}

v1::HistoryCatalog table_contracts(
    const TableStore& store,
    const std::string& snapshot,
    const std::string& source,
    const std::string& contracts_source,
    const std::string& exchange,
    const std::string& product)
{
    const auto lists = store.revisions(
        series_of(contracts_table, contracts_source),
        exchange,
        "all",
        "all",
        held(store, snapshot));
    if (lists.empty())
        throw Error(ErrorCode::conflict, "no contract list of " + exchange + " is stored");
    const auto& table = history_table(contracts_table);
    const auto column = [&](std::string_view name) {
        return table_column(table, name);
    };
    const auto contract = column("contract"), code = column("source_instrument"),
               name = column("name"), listed = column("list_date"),
               delisted = column("delist_date"), multiplier = column("multiplier"),
               per_unit = column("per_unit"), trade_unit = column("trade_unit"),
               quote_unit = column("quote_unit");
    v1::HistoryCatalog result;
    result.set_source(source);
    for (const auto& row : store.rows(lists.front())) {
        const auto& key = std::get<std::string>(row[contract]);
        const auto* first = std::get_if<std::int64_t>(&row[listed]);
        const auto* last = std::get_if<std::int64_t>(&row[delisted]);
        const auto* instrument = std::get_if<std::string>(&row[code]);
        if (HistoryIdentity::parse(key).product != product || !first || !last || !instrument)
            continue;
        auto* item = result.add_items();
        item->set_contract_id(key);
        item->set_source_instrument(*instrument);
        if (const auto* text = std::get_if<std::string>(&row[name]))
            item->set_name(*text);
        item->set_list_date(date_text(*first));
        item->set_delist_date(date_text(*last));
        if (const auto* units = std::get_if<std::int64_t>(&row[multiplier]))
            item->mutable_multiplier()->set_units(*units);
        if (const auto* units = std::get_if<std::int64_t>(&row[per_unit]))
            item->mutable_per_unit()->set_units(*units);
        if (const auto* text = std::get_if<std::string>(&row[trade_unit]))
            item->set_trade_unit(*text);
        if (const auto* text = std::get_if<std::string>(&row[quote_unit]))
            item->set_quote_unit(*text);
    }
    return result;
}

TradedDays traded_days(
    const TableStore& store,
    const v1::DailyTable& table,
    const std::string& contract,
    const std::string& first,
    const std::string& last)
{
    protocol::validate_daily_table(table);
    const auto venue = HistoryIdentity::parse(contract).venue;
    const auto held_days = store.revisions(
        series_of(daily_table, table.daily_source()),
        venue,
        first,
        last,
        held(store, table.snapshot()));
    TradedDays result;
    if (held_days.empty())
        return result;
    result.first = held_days.front().period;
    result.last = held_days.back().period;
    const auto facts =
        daily_facts(store, table, venue, result.first, result.last, {contract}, true);
    if (const auto found = facts.trades.find(contract); found != facts.trades.end())
        for (const auto& [day, trade] : found->second)
            result.traded.push_back(day);
    return result;
}

v1::TableDailyPage table_daily(const TableStore& store, const v1::TableDailyQuery& query)
{
    using chart_indicators::DailyCandle;
    protocol::validate_table_daily_query(query);
    const auto& table = query.daily();
    const auto until = held(store, table.snapshot());
    const auto venue = HistoryIdentity::parse(query.contract()).venue;
    // The days it can have a row on: from its listing to its delisting.
    const auto lists = store.revisions(
        series_of(contracts_table, table.contracts_source()),
        venue,
        "all",
        "all",
        until);
    if (lists.empty())
        throw Error(ErrorCode::conflict, "no contract list of " + venue + " is stored");
    const auto& contracts = history_table(contracts_table);
    const auto name = table_column(contracts, "contract"),
               listed = table_column(contracts, "list_date"),
               delisted = table_column(contracts, "delist_date");
    const auto rows = store.rows(lists.front());
    const auto found = std::ranges::find_if(rows, [&](const auto& cells) {
        return std::get<std::string>(cells[name]) == query.contract();
    });
    if (found == rows.end())
        throw Error(ErrorCode::not_found, "the contract list does not list " + query.contract());
    const auto* first = std::get_if<std::int64_t>(&(*found)[listed]);
    const auto* last = std::get_if<std::int64_t>(&(*found)[delisted]);
    if (!first || !last)
        throw Error(
            ErrorCode::not_found,
            "the contract list gives no listing days of " + query.contract());
    const auto revisions = store.revisions(
        series_of(daily_table, table.daily_source()),
        venue,
        date_text(*first),
        date_text(*last),
        until);
    const auto& daily = history_table(daily_table);
    const std::size_t columns[] = {
        table_column(daily, "contract"),
        table_column(daily, "open"),
        table_column(daily, "high"),
        table_column(daily, "low"),
        table_column(daily, "close"),
        table_column(daily, "volume")
    };
    const std::string prefixes[] = {query.contract()};
    std::map<std::string, DailyCandle> days;
    store.scan(revisions, columns, prefixes, [&](std::size_t revision, auto& cells) {
        if (std::get<std::string>(cells[0]) != query.contract())
            return;
        const auto value = [&](std::size_t cell) {
            return std::get_if<std::int64_t>(&cells[cell]);
        };
        const auto *open = value(1), *high = value(2), *low = value(3), *close = value(4),
                   *volume = value(5);
        // A day without an open, a high and a low is one it did not trade in the
        // auction: there is no candle to draw.
        if (!open || !high || !low || !close || !volume)
            return;
        const auto& day = revisions[revision].period;
        DailyCandle candle{
            parse_trading_date(day),
            Decimal::from_raw(*open),
            Decimal::from_raw(*high),
            Decimal::from_raw(*low),
            Decimal::from_raw(*close),
            Decimal::parse(std::to_string(*volume))
        };
        candle.validate();
        days.emplace(day, candle);
    });
    std::vector<DailyCandle> candles;
    for (const auto& [day, candle] : days)
        candles.push_back(candle);
    if (query.period() != v1::DAY) {
        std::vector<DailyCandle> grouped;
        for (const auto& bucket : chart_indicators::calendar_bars(
                 candles,
                 query.period() == v1::WEEK          ? chart_indicators::CalendarPeriod::week
                     : query.period() == v1::MONTH   ? chart_indicators::CalendarPeriod::month
                     : query.period() == v1::QUARTER ? chart_indicators::CalendarPeriod::quarter
                                                     : chart_indicators::CalendarPeriod::year))
            grouped.push_back(bucket.bar);
        candles = std::move(grouped);
    }
    if (query.offset() > candles.size() || (!candles.empty() && query.offset() == candles.size()))
        throw std::invalid_argument("daily table chart offset is out of range");
    v1::TableDailyPage page;
    *page.mutable_daily() = table;
    page.set_contract(query.contract());
    page.set_period(query.period());
    page.set_total(candles.size());
    page.set_offset(query.offset());
    page.set_limit(query.limit());
    // MACD runs from the first candle, whichever page is asked for.
    chart_indicators::Macd macd;
    for (std::size_t i = 0;
        i < candles.size() && page.candles_size() < static_cast<int>(query.limit());
        ++i) {
        const auto indicator = macd.push(candles[i].close);
        if (i < query.offset())
            continue;
        auto* row = page.add_candles();
        row->set_trading_day(format_trading_date(candles[i].trading_day));
        row->mutable_open()->set_units(candles[i].open.raw());
        row->mutable_high()->set_units(candles[i].high.raw());
        row->mutable_low()->set_units(candles[i].low.raw());
        row->mutable_close()->set_units(candles[i].close.raw());
        row->mutable_volume()->set_units(candles[i].volume.raw());
        if (indicator) {
            row->mutable_macd()->set_diff(indicator->diff);
            row->mutable_macd()->set_signal(indicator->signal);
            row->mutable_macd()->set_histogram(indicator->histogram);
        }
    }
    return page;
}

} // namespace asterion::data
