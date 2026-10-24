#pragma once
// Test-only providers and persistence fixtures. Never linked into product targets.
#include "data/bar_fixture.hpp"
#include "data/history_requests.hpp"
#include "history_minutes.hpp"
#include "history_tables.hpp"
#include "data/data_fixture.hpp"
#include <asterion/domain/history_identity.hpp>
#include <asterion/domain/history_tables.hpp>
#include <algorithm>
#include <cctype>
#include <map>

namespace asterion::test {

// The sources of the market-wide daily table, its calendar and its contract
// list. The test provider's by default; a test of the real Terminal seeds
// under the sources the installed data source offers, which are the ones its
// pages read.
struct DailyTableSources
{
    std::string daily = "fixture.daily.v1", calendar = "fixture.calendar.v1",
                contracts = "fixture.contracts.v1";
};

// What a contract traded at on a day, and what the lots were worth in all
// where the row publishes it.
struct DailyTrade
{
    int open, high, low, close, volume;
    long long amount = 0;
};

// One day of a contract in the daily table. Without a trade nobody traded it
// in the auction: the table then has its settlement, the same price as its
// close, and the lots that changed hands outside the auction, if any (as in a
// delivery month). Without a settlement the source has none for the day, as
// happens on the last day of a contract.
struct DailyRow
{
    std::string day;
    std::optional<int> settlement;
    int open_interest;
    std::optional<DailyTrade> trade = {};
    int off_auction_volume = 0;
};

// Adds one SHFE contract's days to the daily table and to the calendar, and
// the contract to the contract list as `lot` tonnes a lot quoted per tonne,
// listed from the first of the month a year before its delivery month to the
// fifteenth of that month, keeping what other contracts already have there.
// Returns where a dataset reads them, under the snapshot that now holds them.
inline Json seed_daily_table(
    const std::filesystem::path& root,
    const std::string& contract_id,
    const std::vector<DailyRow>& days,
    const DailyTableSources& sources = {},
    int lot = 10)
{
    const history_files::TableSeries daily{"futures.daily", 1, sources.daily},
        calendar{"futures.calendar", 1, sources.calendar},
        contracts{"futures.contracts", 1, sources.contracts};
    history_files::TableStore tables(root / "tables");
    const auto merge = [&](const history_files::TableSeries& series,
                           const std::string& period,
                           const std::vector<TableSourceRow>& added) {
        const auto& table = history_table(series.table);
        std::map<std::string, TableSourceRow> rows; // by key
        if (const auto held = tables.current(series, "SHFE", period))
            for (const auto& cells : tables.rows(*held)) {
                TableSourceRow row;
                for (std::size_t c = 0; c < cells.size(); ++c)
                    row.push_back(table_cell_text(table.columns[c], cells[c]));
                const auto key = row.at(0).value();
                rows[key] = std::move(row);
            }
        for (const auto& row : added)
            rows[row.at(0).value()] = row;
        std::vector<TableSourceRow> all;
        for (auto& [key, row] : rows)
            all.push_back(std::move(row));
        const auto partition = table_partition(table, "SHFE", period, all);
        (void)tables.record(
            partition,
            series,
            tables.write(partition),
            std::string(64, 'a'),
            1790000000000000000);
    };
    std::map<std::string, std::vector<TableSourceRow>> years;
    for (const auto& day : days) {
        years[day.day.substr(0, 4)].push_back({day.day, "1", std::nullopt});
        TableSourceRow row(history_table("futures.daily").columns.size());
        row[0] = contract_id;
        if (day.settlement)
            row[4] = row[5] = std::to_string(*day.settlement);
        row[10] = std::to_string(day.open_interest);
        if (day.trade) {
            row[1] = std::to_string(day.trade->open);
            row[2] = std::to_string(day.trade->high);
            row[3] = std::to_string(day.trade->low);
            row[4] = std::to_string(day.trade->close);
            row[8] = std::to_string(day.trade->volume);
            if (day.trade->amount)
                row[9] = std::to_string(day.trade->amount);
        } else if (day.off_auction_volume) {
            row[8] = std::to_string(day.off_auction_volume);
        }
        merge(daily, day.day, {row});
    }
    for (const auto& [year, open] : years)
        merge(calendar, year, open);
    TableSourceRow listed(history_table("futures.contracts").columns.size());
    listed[0] = contract_id;
    const auto delivery = HistoryIdentity::parse(contract_id).delivery_month;
    listed[3] = std::to_string(std::stoi(delivery.substr(0, 4)) - 1) + delivery.substr(4) + "-01";
    listed[4] = delivery + "-15";
    listed[7] = std::to_string(lot);
    listed[8] = "吨";
    listed[9] = "人民币元/吨";
    merge(contracts, "all", {listed});
    return {
        {"snapshot", tables.head()},
        {"daily_source", daily.source},
        {"calendar_source", calendar.source},
        {"contracts_source", contracts.source}
    };
}

// Every day settles at `settlement` with `open_interest` held, having traded
// from 100 between 90 and 201 to 110: prices whose increment is 1.
inline Json seed_daily_table(
    const std::filesystem::path& root,
    const std::string& contract_id,
    const std::vector<std::string>& days,
    int settlement,
    int open_interest,
    const DailyTableSources& sources = {})
{
    std::vector<DailyRow> rows;
    for (const auto& day : days)
        rows.push_back({day, settlement, open_interest, DailyTrade{100, 201, 90, 110, 10000}});
    return seed_daily_table(root, contract_id, rows, sources);
}

// Of several seeded histories, the daily table as the last of them left it:
// every seeding keeps what the earlier ones stored.
inline Json latest_daily(const std::vector<Json>& selections)
{
    return std::ranges::max_element(selections, {}, [](const Json& selection) {
        return selection.at("daily_sequence").get<std::uint64_t>();
    })->at("daily");
}

// Minute bars spread evenly over `minute_days` (09:00 onwards, bar end, each
// with its trading day), published as one download; the contract's settlement
// 110 on `daily_days` in the market-wide daily table, which is where a dataset
// reads it, and the contract in the contract list at ten tonnes a lot.
// The contract is SHFE `product` of delivery `month`, e.g. rb2610; every daily
// row reports `open_interest`.
inline Json seed_history(
    const std::filesystem::path& root,
    const std::vector<int>& prices,
    const std::string& id,
    std::vector<std::string> minute_days = {"2026-09-25"},
    std::vector<std::string> daily_days = {},
    const std::string& product = "rb",
    unsigned interval = 1,
    int settlement_price = 110,
    const std::string& normalization = "test.confirmed.v1",
    const std::string& month = "2026-10",
    int open_interest = 100,
    const std::string& data_instance = "historical-data",
    const DailyTableSources& tables = {})
{
    if (daily_days.empty())
        daily_days = minute_days;

    class Minutes final : public HistoricalBarPort
    {
    public:
        std::vector<int> prices;
        std::string normalization;
        std::vector<std::string> days;

        HistorySemantics semantics() const override
        {
            return {"tushare.ft_mins", normalization, "Asia/Shanghai", "bar_end"};
        }

        std::vector<HistoricalBar> read(const HistoricalBarRange& r, std::stop_token) override
        {
            std::vector<HistoricalBar> rows;
            const auto per_day = (prices.size() + days.size() - 1) / days.size();
            for (std::size_t index = 0; index < days.size(); ++index) {
                const auto base = parse_shanghai_time(days[index] + " 09:00:00");
                if (base > r.end_ns
                    || base + static_cast<std::int64_t>(per_day - 1) * 60000000000LL < r.begin_ns)
                    continue;
                const auto begin = index * per_day;
                const auto end = std::min(prices.size(), begin + per_day);
                for (auto i = begin; i < end; ++i) {
                    const auto at = base + static_cast<std::int64_t>(i - begin) * 60000000000LL;
                    if (at < r.begin_ns || at > r.end_ns)
                        continue;
                    const auto price = Decimal::parse(std::to_string(prices[i]));
                    rows.push_back(
                        {at,
                            price,
                            price,
                            price,
                            price,
                            dec("10"),
                            dec("1000"),
                            dec("100"),
                            days[index]});
                }
            }
            return rows;
        }
    } minutes;

    if (prices.empty() || prices.size() > 200000)
        throw std::invalid_argument("invalid test prices");
    minutes.normalization = normalization;
    minutes.prices = prices;
    minutes.days = minute_days;
    const auto contract_id = "SHFE/" + product + "/" + month;
    auto upper = product;
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) {
        return std::toupper(c);
    });
    const auto source_instrument = upper + month.substr(2, 2) + month.substr(5) + ".SHF";
    const auto per_day =
        static_cast<std::int64_t>((prices.size() + minute_days.size() - 1) / minute_days.size());
    const auto begin = parse_shanghai_time(minute_days.front() + " 09:00:00");
    const auto end =
        parse_shanghai_time(minute_days.back() + " 09:00:00") + (per_day - 1) * 60000000000LL;
    const auto minute = asterion::testing_support::minute_request(
        {{"contract_id", contract_id},
            {"source", "tushare.ft_mins"},
            {"source_instrument", source_instrument},
            {"interval_minutes", interval},
            {"begin_ns", std::to_string(begin)},
            {"end_ns", std::to_string(end)},
            {"requests_per_minute", 500}});
    std::string bars;
    {
        // One process owns the table ledger: the store goes before the tables open.
        data::Store store(root, data_instance);
        bars = publish_download(store, id + "-bars", minute, minutes)
                   .minute_result()
                   .manifest_sha256();
    }
    const auto table =
        seed_daily_table(root, contract_id, daily_days, settlement_price, open_interest, tables);
    return {
        {"source_dataset_ids", Json::array({bars})},
        {"daily", table},
        {"daily_sequence",
            history_files::TableStore(root / "tables")
                .sequence(table.at("snapshot").get<std::string>())
                .value()},
        {"begin_day", ""},
        {"end_day", ""}
    };
}

} // namespace asterion::test
