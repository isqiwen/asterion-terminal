#include "support/data_discovery.hpp"
#include <asterion/research.pb.h>
#include "support/task_wait.hpp"
#include "../plugins/data/tushare/margin_definition.hpp"
#include <asterion/domain/account.hpp>
#include "data_store.hpp"
#include "table_series.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/task.hpp>
#include <asterion/task.pb.h>
#include <gtest/gtest.h>
#include <fstream>
#include <thread>
using namespace asterion;
using history_files::TableSeries;
using history_files::TableStore;

namespace {

struct Folder
{
    std::filesystem::path path;

    explicit Folder(const char* prefix)
        : path(std::filesystem::path("/tmp") / (prefix + unique_process_id().substr(0, 12)))
    {
        std::filesystem::create_directory(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    }

    ~Folder()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

const TableSeries daily{"futures.daily", 1, "fixture.daily.v1"},
    calendar{"futures.calendar", 1, "fixture.calendar.v1"},
    contracts{"futures.contracts", 1, "fixture.contracts.v1"};
const std::string artifact(64, 'a');

std::string keep(
    TableStore& store,
    const TableSeries& series,
    const std::string& period,
    const std::vector<TableSourceRow>& rows,
    std::int64_t acquired)
{
    const auto partition = table_partition(history_table(series.table), "SHFE", period, rows);
    return store.record(partition, series, store.write(partition), artifact, acquired)
        .revision.snapshot;
}

// A day of one contract: what it closed and settled at, and how much was held.
TableSourceRow traded(
    const char* contract,
    const char* close,
    const std::string& settlement,
    const std::string& open_interest)
{
    TableSourceRow cells(history_table("futures.daily").columns.size());
    cells[0] = contract;
    if (close)
        cells[4] = close;
    cells[5] = settlement;
    cells[10] = open_interest;
    return cells;
}

// The first week of March 2026 and the Monday after it.
const char* const days[] =
    {"2026-03-02", "2026-03-03", "2026-03-04", "2026-03-05", "2026-03-06", "2026-03-09"};

void keep_calendar(TableStore& store)
{
    std::vector<TableSourceRow> year;
    for (const auto* day : days)
        year.push_back({day, "1", std::nullopt});
    year.push_back({"2026-03-07", "0", std::nullopt});
    year.push_back({"2026-03-08", "0", std::nullopt});
    (void)keep(store, calendar, "2026", year, 900);
}

// Rebar's May month loses its open interest to October on the third day.
std::string keep_rebar(TableStore& store, int except = -1)
{
    const char* near[] = {"3100", "3110", "3120", "3130", nullptr, nullptr};
    const char* held[][2] = {
        {"500", "100"},
        {"400", "300"},
        {"200", "600"},
        {"100", "700"},
        {"50", "800"},
        {"10", "900"}
    };
    std::string snapshot;
    for (int d = 0; d < 6; ++d) {
        if (d == except)
            continue;
        const auto settled = std::to_string(3100 + 10 * d), far = std::to_string(3200 + 10 * d);
        snapshot = keep(
            store,
            daily,
            days[d],
            {traded("SHFE/rb/2026-05", near[d], settled, held[d][0]),
                traded("SHFE/rb/2026-10", far.c_str(), far, held[d][1]),
                traded("SHFE/cu/2026-04", nullptr, "70000", "0")},
            1000 + d);
    }
    return snapshot;
}

data::proto::TableSeriesRequest request(
    const std::string& snapshot,
    std::vector<std::string> instruments,
    const char* begin = "2026-03-02",
    const char* end = "2026-03-09")
{
    data::proto::TableSeriesRequest result;
    result.set_snapshot(snapshot);
    result.set_daily_source(daily.source);
    result.set_calendar_source(calendar.source);
    result.set_begin_day(begin);
    result.set_end_day(end);
    for (auto& instrument : instruments)
        result.add_instruments(std::move(instrument));
    return result;
}

std::vector<std::pair<std::string, std::string>> closes(const data::proto::TableMonth& month)
{
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& close : month.closes())
        result.emplace_back(close.trading_day(), Decimal::from_raw(close.close().units()).str());
    return result;
}

// A contract in the contract list, listed and delisted on these days.
TableSourceRow listing(const char* contract, const char* listed, const char* delisted)
{
    TableSourceRow cells(history_table("futures.contracts").columns.size());
    cells[0] = contract;
    cells[3] = listed;
    cells[4] = delisted;
    return cells;
}

// Rebar's October month as a chart draws it: 45 weekdays from 2026-01-05, the
// day the contract list lists it, on the eleventh of which (2026-01-19)
// nobody traded it in the auction. Day i opens at 3000 + i, ranges ten either
// side, closes five above the open and trades 100 + i lots. The Friday before
// the listing holds a row of it too, as copper's month is on every day.
std::string keep_chart(TableStore& store)
{
    using namespace std::chrono;
    (void)keep(
        store,
        contracts,
        "all",
        {listing("SHFE/cu/2026-04", "2025-04-16", "2026-04-15"),
            listing("SHFE/rb/2026-10", "2026-01-05", "2026-03-27")},
        900);
    const auto row = [](const char* contract, int open, int volume) {
        TableSourceRow cells(history_table("futures.daily").columns.size());
        cells[0] = contract;
        for (const auto [cell, price] :
            {std::pair{1, open}, {2, open + 10}, {3, open - 10}, {4, open + 5}, {5, open + 5}})
            cells[static_cast<std::size_t>(cell)] = std::to_string(price);
        cells[8] = std::to_string(volume);
        return cells;
    };
    std::string snapshot = keep(
        store,
        daily,
        "2026-01-02",
        {row("SHFE/cu/2026-04", 70000, 5), row("SHFE/rb/2026-10", 2990, 90)},
        1000);
    int i = 0;
    for (sys_days day = sys_days(year{2026} / January / 5); i < 45; day += std::chrono::days{1}) {
        if (weekday(day).iso_encoding() > 5)
            continue;
        auto rebar = row("SHFE/rb/2026-10", 3000 + i, 100 + i);
        if (i == 10)
            rebar = traded("SHFE/rb/2026-10", "3015", "3015", "100");
        snapshot = keep(
            store,
            daily,
            format_trading_date(year_month_day(day)),
            {row("SHFE/cu/2026-04", 70000 + i, 5), std::move(rebar)},
            1001 + i);
        ++i;
    }
    return snapshot;
}

data::proto::TableDailyQuery chart(
    const std::string& snapshot,
    data::proto::DailyPeriod period,
    std::uint64_t offset = 0,
    std::uint32_t limit = 200,
    const char* contract = "SHFE/rb/2026-10")
{
    data::proto::TableDailyQuery query;
    auto& table = *query.mutable_daily();
    table.set_snapshot(snapshot);
    table.set_daily_source(daily.source);
    table.set_calendar_source(calendar.source);
    table.set_contracts_source(contracts.source);
    query.set_contract(contract);
    query.set_period(period);
    query.set_offset(offset);
    query.set_limit(limit);
    return query;
}

template <class Call>
std::string refusal(Call&& call)
{
    try {
        call();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

} // namespace

TEST(HistoricalMargin, PinsRawSnapshotAndRejectsUnprovedVendorUnitsAndTime)
{
    Folder folder("ast-margin-evidence-");
    auto store = std::make_unique<TableStore>(folder.path);
    const TableSeries settlement{"futures.settlement", 1, "tushare.fut_settle.v2"};
    const auto row = [](const char* long_rate, const char* short_rate) {
        TableSourceRow cells(history_table("futures.settlement").columns.size());
        cells[0] = "SHFE/rb/2026-10";
        if (long_rate)
            cells[5] = long_rate;
        if (short_rate)
            cells[6] = short_rate;
        return cells;
    };
    const auto original = keep(*store, settlement, "2026-03-02", {row("12", "12")}, 1000);
    const auto fact =
        data::settlement_margin_fact(*store, original, settlement, "SHFE/rb/2026-10", "2026-03-02");
    EXPECT_EQ(fact.long_rate->str(), "12");
    EXPECT_EQ(fact.snapshot, original);
    EXPECT_EQ(fact.source, settlement.source);
    EXPECT_EQ(fact.provider_artifact, artifact);
    EXPECT_THROW(
        data::settlement_margin_fact(
            *store,
            original,
            settlement,
            "SHFE/rb/2027-01",
            fact.observed_day),
        std::invalid_argument);
    EXPECT_FALSE(fact.content.empty());
    const auto definition = tushare::settlement_margin_definition("SHFE");
    EXPECT_NE(
        refusal([&] {
            (void)historical_costs::pin_margin(fact, definition, "2026-03-02");
        }).find("unit evidence is missing"),
        std::string::npos);
    const auto revised = keep(*store, settlement, "2026-03-02", {row("18", nullptr)}, 1100);
    EXPECT_EQ(
        data::settlement_margin_fact(*store, original, settlement, fact.contract, fact.observed_day)
            .long_rate->str(),
        "12");
    const auto latest =
        data::settlement_margin_fact(*store, revised, settlement, fact.contract, fact.observed_day);
    EXPECT_EQ(latest.long_rate->str(), "18");
    EXPECT_FALSE(latest.short_rate);
    const auto zero_snapshot = keep(*store, settlement, "2026-03-02", {row("0", "0")}, 1200);
    const auto zero = data::settlement_margin_fact(
        *store,
        zero_snapshot,
        settlement,
        fact.contract,
        fact.observed_day);
    ASSERT_TRUE(zero.long_rate);
    EXPECT_EQ(zero.long_rate->str(), "0");

    EXPECT_NE(latest.content, fact.content);
    EXPECT_EQ(fact.long_rate->str(), "12");
    EXPECT_THROW(
        data::settlement_margin_fact(*store, original, settlement, fact.contract, "2026-03-03"),
        std::invalid_argument);
    EXPECT_THROW(
        data::settlement_margin_fact(
            *store,
            std::string(64, 'f'),
            settlement,
            fact.contract,
            fact.observed_day),
        std::invalid_argument);
    store.reset();
    TableStore reopened(folder.path);
    const auto restored = data::settlement_margin_fact(
        reopened,
        original,
        settlement,
        fact.contract,
        fact.observed_day);
    EXPECT_EQ(restored.snapshot, fact.snapshot);
    EXPECT_EQ(restored.content, fact.content);
    EXPECT_EQ(restored.object, fact.object);
    EXPECT_EQ(restored.provider_artifact, fact.provider_artifact);
    EXPECT_EQ(restored.long_rate, fact.long_rate);
    EXPECT_EQ(restored.short_rate, fact.short_rate);
}

TEST(HistoricalMargin, ExplicitFixtureDefinitionConvertsExactlyAndDoesNotInventAvailabilityOrFees)
{
    using namespace historical_costs;
    MarginFact fact{
        "SHFE/rb/2026-10",
        "2026-03-02",
        "fixture.margin.v1",
        std::string(64, 'a'),
        std::string(64, 'b'),
        std::string(64, 'c'),
        std::string(64, 'd'),
        std::string(64, 'e'),
        1000,
        Decimal::parse("12"),
        Decimal::parse("12")
    };
    MarginDefinition definition{
        fact.source,
        "SHFE",
        MarginUnit::percent,
        MarginPhase::trading_day_start,
        MarginBasis::exchange_speculation,
        "fixture://percent-definition",
        "fixture://effective-at-day-start",
        "fixture://exchange-reference"
    };
    const auto pinned = pin_margin(fact, definition, "2026-03-03");
    EXPECT_THROW(pin_margin(fact, definition, ""), std::invalid_argument);
    auto no_reference = definition;
    no_reference.effective_reference.clear();
    EXPECT_THROW(pin_margin(fact, no_reference, "2026-03-03"), std::invalid_argument);

    EXPECT_EQ(pinned.long_rate.str(), "0.12");
    EXPECT_EQ(pinned.effective_day, "2026-03-03");
    EXPECT_EQ(pinned.fact.long_rate->str(), "12");
    EXPECT_EQ(pinned.fact.snapshot, fact.snapshot);
    EXPECT_EQ(pinned.definition.effective_reference, definition.effective_reference);
    EXPECT_FALSE(pinned.published_at_ns);
    auto other_exchange = fact;
    other_exchange.contract = "DCE/m/2026-09";
    EXPECT_THROW(pin_margin(other_exchange, definition, "2026-03-03"), std::invalid_argument);

    EXPECT_THROW(require_margin_known_by(pinned, 2000), std::invalid_argument);
    auto known = pinned;
    known.published_at_ns = 2000;
    known.publication_reference = "fixture://publication";
    EXPECT_THROW(require_margin_known_by(known, 1999), std::invalid_argument);
    EXPECT_NO_THROW(require_margin_known_by(known, 2000));
    FuturesCosts configured;
    configured.margin_rate = trading_day_start_margin(pinned, MarginBasis::exchange_speculation);
    configured.open_fee = Decimal::parse("2");
    configured.close_today_fee = Decimal::parse("3");
    configured.close_yesterday_fee = Decimal::parse("4");
    EXPECT_NO_THROW(configured.validate());
    EXPECT_EQ(
        configured.margin(Decimal::parse("2"), Decimal::parse("100"), Decimal::parse("10")).str(),
        "240");
    EXPECT_EQ(
        configured.open_fee.str(),
        "2"); // Explicit costs stay explicit, never inferred from settlement fields.
    EXPECT_THROW(
        trading_day_start_margin(pinned, MarginBasis::broker_actual),
        std::invalid_argument);
    definition.phase = MarginPhase::settlement;
    const auto at_settlement = pin_margin(fact, definition, "2026-03-02");
    EXPECT_THROW(
        trading_day_start_margin(at_settlement, MarginBasis::exchange_speculation),
        std::invalid_argument);
    definition.phase = MarginPhase::unknown;
    EXPECT_THROW(pin_margin(fact, definition, "2026-03-03"), std::invalid_argument);
    definition.phase = MarginPhase::trading_day_start;
    fact.short_rate = Decimal::parse("15");
    EXPECT_THROW(
        trading_day_start_margin(
            pin_margin(fact, definition, "2026-03-03"),
            MarginBasis::exchange_speculation),
        std::invalid_argument);
    fact.short_rate.reset();
    EXPECT_THROW(pin_margin(fact, definition, "2026-03-03"), std::invalid_argument);
    fact.short_rate = fact.long_rate = Decimal::parse("0.00000001");
    EXPECT_THROW(pin_margin(fact, definition, "2026-03-03"), std::invalid_argument);
    fact.long_rate = fact.short_rate = Decimal::parse("0.12");
    definition.unit = MarginUnit::fraction;
    EXPECT_EQ(pin_margin(fact, definition, "2026-03-03").long_rate.str(), "0.12");
}

// Public official rows are an offline regression fixture, never a production
// provider or an assertion about when these historical bytes were first published.
TEST(HistoricalMargin, OfficialAu2512KeepsTradingParametersSeparateFromSettlement)
{
    using namespace historical_costs;
    std::ifstream input(
        std::filesystem::path(__FILE__).parent_path() / "fixtures/shfe_au2512_margin.json");
    ASSERT_TRUE(input);
    const auto evidence = Json::parse(input);
    const std::string contract = evidence.at("contract"), source = evidence.at("fixture_source");
    const TableSeries settlement{"futures.settlement", 1, source};
    Folder folder("ast-shfe-au-margin-");
    TableStore store(folder.path);
    std::vector<MarginFact> facts;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& event = evidence.at("events").at(i);
        const std::string long_rate = event.at("long_rate"), short_rate = event.at("short_rate");
        const auto& raw = event.at("raw_au2512_row");
        EXPECT_EQ(raw.at(event.at("long_field").get<std::string>()), long_rate);
        EXPECT_EQ(raw.at(event.at("short_field").get<std::string>()), short_rate);
        TableSourceRow cells(history_table("futures.settlement").columns.size());
        cells[0] = contract;
        cells[5] = long_rate;
        cells[6] = short_rate;
        const std::string day = event.at("day");
        const auto snapshot = keep(store, settlement, day, {cells}, 1000 + i);
        facts.push_back(data::settlement_margin_fact(store, snapshot, settlement, contract, day));
        if (event.at("kind") == "settlement") {
            const std::string raw_reference = event.at("source").at("url").get<std::string>()
                + "#sha256=" + event.at("source").at("sha256").get<std::string>();
            const MarginDefinition definition{
                source,
                "SHFE",
                MarginUnit::fraction,
                MarginPhase::settlement,
                MarginBasis::exchange_speculation,
                evidence.at("unit_reference"),
                raw_reference,
                evidence.at("unit_reference")
            };
            const auto pinned = pin_margin(facts.back(), definition, day);
            EXPECT_EQ(pinned.long_rate.str(), long_rate);
            EXPECT_EQ(pinned.short_rate.str(), short_rate);
            EXPECT_EQ(pinned.effective_day, day);
            EXPECT_FALSE(pinned.published_at_ns);
            EXPECT_THROW(require_margin_known_by(pinned, 2000), std::invalid_argument);
            EXPECT_EQ(
                refusal([&] {
                    (void)trading_day_start_margin(pinned, MarginBasis::exchange_speculation);
                }),
                "settlement margin cannot be applied at trading-day start");
        }
    }
    // Same calendar label, different parameter tables and applicability. Neither
    // the next day's row nor a URL's update_date grants an automatic shift rule.
    EXPECT_EQ(facts[0].long_rate->str(), "0.14"); // Oct 20 settlement
    EXPECT_EQ(facts[1].long_rate->str(), "0.14"); // Oct 21 trading parameters
    EXPECT_EQ(facts[2].long_rate->str(), "0.16"); // Oct 21 settlement
    EXPECT_EQ(facts[3].long_rate->str(), "0.16"); // Oct 22 trading parameters
    EXPECT_EQ(facts[1].observed_day, facts[2].observed_day);
    EXPECT_NE(facts[1].snapshot, facts[2].snapshot);
    EXPECT_NE(facts[1].content, facts[2].content);
    const auto old = data::settlement_margin_fact(
        store,
        facts[1].snapshot,
        settlement,
        contract,
        facts[1].observed_day);
    EXPECT_EQ(old.long_rate->str(), "0.14");
    const auto& event = evidence.at("events").at(2);
    const auto& duplicate_report = evidence.at("events").at(4);
    EXPECT_EQ(duplicate_report.at("long_rate"), event.at("long_rate"));
    EXPECT_NE(duplicate_report.at("update_date"), event.at("update_date"));
    EXPECT_EQ(event.at("raw_au2512_row").at("LONGMARGINRATIO"), "0.15");
    EXPECT_EQ(event.at("raw_au2512_row").at("SETTLEMENTPRICE"), "991.44");
    EXPECT_FALSE(evidence.at("publication_known").get<bool>());
    // SHFE's representation does not establish Tushare's units/date mapping.
    const auto vendor = tushare::settlement_margin_definition("SHFE");
    EXPECT_EQ(vendor.unit, MarginUnit::unknown);
    EXPECT_EQ(vendor.phase, MarginPhase::unknown);
}

TEST(TableSeries, ReplayAnchorUsesStoredCalendarAndRefusesUnknownPreviousDay)
{
    Folder folder("ast-replay-calendar-");
    TableStore store(folder.path);
    keep_calendar(store);
    const auto snapshot = keep_rebar(store);
    const auto table = chart(snapshot, static_cast<data::proto::DailyPeriod>(0)).daily();
    EXPECT_EQ(data::previous_trading_day(store, table, "SHFE", "2026-03-09"), "2026-03-06");
    EXPECT_NE(
        refusal([&] {
            (void)data::previous_trading_day(store, table, "SHFE", "2026-03-07");
        }).find("dominant replay anchor is not an open trading day"),
        std::string::npos);
    EXPECT_FALSE(refusal([&] {
        (void)data::previous_trading_day(store, table, "SHFE", "2026-03-02");
    }).empty());
}

TEST(TableSeries, DaysOnWhichAnExchangeHeldTheSameRowsAreEachRead)
{
    // Nothing moved from one day to the next: the two partitions have the same
    // rows and so the same file, and each day still has them.
    Folder folder("ast-same-");
    TableStore store(folder.path);
    keep_calendar(store);
    std::string snapshot;
    for (const auto* day : {days[0], days[1]})
        snapshot =
            keep(store, daily, day, {traded("SHFE/rb/2026-10", "3200", "3200", "100")}, 1000);
    const auto first = store.current(daily, "SHFE", days[0]),
               second = store.current(daily, "SHFE", days[1]);
    ASSERT_TRUE(first && second);
    ASSERT_EQ(first->object, second->object);
    const auto series =
        data::table_series(store, request(snapshot, {"SHFE/rb/2026-10"}, days[0], days[1]))
            .series(0);
    ASSERT_EQ(series.months_size(), 1);
    EXPECT_EQ(
        closes(series.months(0)),
        (std::vector<std::pair<std::string, std::string>>{{days[0], "3200"}, {days[1], "3200"}}));
}

TEST(TableSeries, AProductIsReadThroughItsDominantMonthsUnderOneSnapshot)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    keep_calendar(store);
    const auto snapshot = keep_rebar(store);
    const auto group = data::table_series(store, request(snapshot, {"SHFE/rb", "SHFE/rb/2026-05"}));
    ASSERT_EQ(group.series_size(), 2);
    const auto& product = group.series(0);
    EXPECT_EQ(product.instrument(), "SHFE/rb");
    EXPECT_EQ(product.acquired_at_ns(), 1005) << "the most recent of the days read";
    // The first day only says what was held; May leads until October holds more.
    ASSERT_EQ(product.months_size(), 2);
    EXPECT_EQ(product.months(0).contract(), "SHFE/rb/2026-05");
    EXPECT_EQ(
        closes(product.months(0)),
        (std::vector<std::pair<std::string, std::string>>{
            {"2026-03-03", "3110"},
            {"2026-03-04", "3120"}
        }));
    EXPECT_EQ(product.months(1).contract(), "SHFE/rb/2026-10");
    EXPECT_EQ(
        closes(product.months(1)),
        (std::vector<std::pair<std::string, std::string>>{
            {"2026-03-05", "3230"},
            {"2026-03-06", "3240"},
            {"2026-03-09", "3250"}
        }));
    const auto& schedule = product.schedule();
    ASSERT_EQ(schedule.rolls_size(), 2);
    EXPECT_EQ(schedule.rolls(0).trading_day(), "2026-03-03");
    // October over May at the settlements of the day before the roll: 3220 / 3120.
    EXPECT_EQ(Decimal::from_raw(schedule.rolls(0).factor().units()).str(), "1.03205128");
    EXPECT_EQ(schedule.rolls(1).trading_day(), "2026-03-05");
    EXPECT_EQ(Decimal::from_raw(schedule.rolls(1).factor().units()).str(), "1");
    ASSERT_EQ(schedule.terms_size(), 2) << "October has no later month to compare with";
    EXPECT_EQ(schedule.terms(1).trading_day(), "2026-03-04");
    EXPECT_EQ(Decimal::from_raw(schedule.terms(1).near().units()).str(), "3110");
    EXPECT_EQ(Decimal::from_raw(schedule.terms(1).far().units()).str(), "3210");
    EXPECT_EQ(schedule.terms(1).far_month(), "2026-10");
    // A contract is its own closes; the days it did not trade are not there.
    const auto& contract = group.series(1);
    ASSERT_EQ(contract.months_size(), 1);
    EXPECT_FALSE(contract.has_schedule());
    EXPECT_EQ(closes(contract.months(0)).size(), 4U);
    EXPECT_EQ(
        closes(contract.months(0)).back(),
        (std::pair<std::string, std::string>{"2026-03-05", "3130"}));

    // What a factor reads of them: the product at the level of its last month.
    factor::proto::FactorInput input;
    input.set_factor(protocol::proto::PRICE_MOMENTUM);
    *input.add_series()->mutable_table() = product;
    const auto observed = protocol::factor_observations(input);
    ASSERT_EQ(observed.closes.at(0).size(), 5U);
    EXPECT_EQ(observed.closes[0][0]->str(), "3209.6794808") << "3110 x 1.03205128";
    EXPECT_EQ(observed.closes[0][2]->str(), "3230");
    input.set_factor(protocol::proto::TERM_STRUCTURE);
    const auto carried = protocol::factor_observations(input);
    ASSERT_EQ(carried.terms.at(0).size(), 2U) << "only the days that have a term point";
    // (3110 / 3210 - 1) x 12 / 5 months apart.
    EXPECT_EQ(carried.terms[0][1]->str(), "-0.07476636");

    // A later correction is another snapshot; the first still holds what it held.
    const auto corrected = keep(
        store,
        daily,
        "2026-03-03",
        {traded("SHFE/rb/2026-05", "3111", "3110", "400"),
            traded("SHFE/rb/2026-10", "3210", "3210", "300")},
        2000);
    const auto again = data::table_series(store, request(snapshot, {"SHFE/rb", "SHFE/rb/2026-05"}));
    EXPECT_EQ(again.SerializeAsString(), group.SerializeAsString());
    const auto revised = data::table_series(store, request(corrected, {"SHFE/rb"})).series(0);
    EXPECT_EQ(closes(revised.months(0)).front().second, "3111");
    EXPECT_EQ(revised.acquired_at_ns(), 2000);
    EXPECT_NE(protocol::table_series_revision(revised), protocol::table_series_revision(product));
}

TEST(TableSeries, AMonthStillDominantOnItsLastDayHandsOverOnTheNext)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    keep_calendar(store);
    // April holds the most until it is gone; nothing of it is left on the fourth day.
    std::string snapshot;
    for (int d = 0; d < 6; ++d) {
        const auto far = std::to_string(72000 + 100 * d);
        std::vector<TableSourceRow> rows{traded("SHFE/cu/2026-06", far.c_str(), far, "100")};
        if (d < 3) {
            const auto near = std::to_string(70000 + 100 * d);
            rows.push_back(traded("SHFE/cu/2026-04", near.c_str(), near, "500"));
        }
        snapshot = keep(store, daily, days[d], rows, 1000 + d);
    }
    const auto series = data::table_series(store, request(snapshot, {"SHFE/cu"})).series(0);
    ASSERT_EQ(series.months_size(), 2);
    EXPECT_EQ(
        closes(series.months(0)).back(),
        (std::pair<std::string, std::string>{"2026-03-04", "70200"}));
    EXPECT_EQ(series.schedule().rolls(1).trading_day(), "2026-03-05");
    // June over April at the settlements of April's last day: 72200 / 70200.
    EXPECT_EQ(Decimal::from_raw(series.schedule().rolls(0).factor().units()).str(), "1.02849003");
    EXPECT_EQ(
        closes(series.months(1)).front(),
        (std::pair<std::string, std::string>{"2026-03-05", "72300"}));
}

TEST(TableSeries, AHoleInTheRangeIsRefusedNotReadAround)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    keep_calendar(store);
    const auto snapshot = keep_rebar(store, 2);
    const auto ask = [&](data::proto::TableSeriesRequest request) {
        return refusal([&] {
            (void)data::table_series(store, request);
        });
    };
    EXPECT_EQ(
        ask(request(snapshot, {"SHFE/rb"})),
        "futures.daily is missing trading days of the range: SHFE 2026-03-04");
    // Without the missing day the range is whole, and each instrument answers for itself.
    EXPECT_EQ(ask(request(snapshot, {"SHFE/rb"}, "2026-03-05")), "");
    EXPECT_EQ(
        ask(request(snapshot, {"SHFE/rb", "SHFE/ag"}, "2026-03-05")),
        "instruments cannot be read: SHFE/ag: it has no closes in the range");
    // Every instrument that cannot be read is named, in the order asked for.
    EXPECT_EQ(
        ask(request(snapshot, {"SHFE/cu/2026-04", "SHFE/rb", "SHFE/ag"}, "2026-03-05")),
        "instruments cannot be read: SHFE/cu/2026-04: it has no closes in the range; " "SHFE/ag: it has no closes in the range");
    EXPECT_EQ(
        ask(request(snapshot, {"SHFE/rb", "SHFE/ag"}, "2026-03-09")),
        "instruments cannot be read: SHFE/rb: a dominant series requires at least two " "trading days; SHFE/ag: it has no closes in the range");
    EXPECT_EQ(
        ask(request(snapshot, {"SHFE/rb"}, "2026-12-30", "2027-01-05")),
        "the calendar of the range is not stored: futures.calendar SHFE 2027");
    EXPECT_EQ(
        ask(request(std::string(64, 'b'), {"SHFE/rb"})),
        "the table snapshot is not in this store");
    auto twice = request(snapshot, {"SHFE/rb", "SHFE/rb"});
    EXPECT_EQ(ask(twice), "invalid table series request");
    EXPECT_EQ(ask(request(snapshot, {"SHFE/rb/2026"})), "invalid table instrument");
    // One request asks for at most a hundred instruments, and a factor compares
    // at most as many series however its request is put together.
    std::vector<std::string> many;
    for (int month = 0; month < 101; ++month) {
        char name[24];
        std::snprintf(name, sizeof name, "SHFE/rb/%04d-%02d", 2030 + month / 12, month % 12 + 1);
        many.emplace_back(name);
    }
    EXPECT_EQ(ask(request(snapshot, many)), "invalid table series request");
    many.pop_back();
    // Of a long list the first twenty are named and the rest counted.
    const auto none = ask(request(snapshot, many, "2026-03-05"));
    EXPECT_TRUE(none.starts_with(
        "instruments cannot be read: SHFE/rb/2030-01: it has no closes in the range; "));
    EXPECT_TRUE(none.ends_with("SHFE/rb/2031-08: it has no closes in the range and 80 more"))
        << none;
    const auto sixty = [&](std::size_t first) {
        return Json{
            {"tables",
                {{"snapshot", snapshot},
                    {"daily_source", daily.source},
                    {"calendar_source", calendar.source},
                    {"begin_day", "2026-03-05"},
                    {"end_day", "2026-03-09"},
                    {"instruments",
                        std::vector<std::string>(
                            many.begin() + static_cast<std::ptrdiff_t>(first),
                            many.begin() + static_cast<std::ptrdiff_t>(first) + 60)}}}
        };
    };
    EXPECT_EQ(
        refusal([&] {
            (void)protocol::encode_factor_request(
                {{"series", {sixty(0), sixty(40)}},
                    {"factor", "momentum"},
                    {"lookbacks", {5}},
                    {"horizon", 1},
                    {"evaluation", {{"mode", "full_sample"}}}});
        }),
        "factor analysis studies one series or 3..100 contracts");
}

TEST(TableSeries, ProductsAreThoseOfTheContractListsOfASnapshot)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    const auto snapshot = keep(
        store,
        contracts,
        "all",
        {listing("SHFE/rb/2026-10", "2025-10-16", "2026-10-15"),
            listing("SHFE/rb/2026-05", "2025-05-16", "2026-05-15"),
            listing("SHFE/cu/2026-04", "2025-04-16", "2026-04-15")},
        1000);
    data::proto::TableProductsQuery query;
    query.set_snapshot(snapshot);
    query.set_contracts_source(contracts.source);
    EXPECT_EQ(
        protocol::decode_table_products(data::table_products(store, query)),
        (Json::array(
            {{{"exchange", "SHFE"},
                 {"product", "cu"},
                 {"contracts", 1},
                 {"first_listed", "2025-04-16"},
                 {"last_delisted", "2026-04-15"}},
                {{"exchange", "SHFE"},
                    {"product", "rb"},
                    {"contracts", 2},
                    {"first_listed", "2025-05-16"},
                    {"last_delisted", "2026-10-15"}}})));
}

// What a download picks from: one product's contracts that have a listing
// day, a delisting day and the source's code, with the units the list gives.
TEST(TableSeries, AContractListGivesTheContractsOfAProductThatCanBeDownloaded)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    auto rebar = listing("SHFE/rb/2026-10", "2025-10-16", "2026-10-15");
    rebar[1] = "RB2610.SHF";
    rebar[2] = "螺纹钢2610";
    rebar[7] = "10";
    rebar[8] = "吨";
    rebar[9] = "元/吨";
    auto undated = listing("SHFE/rb/2026-11", "2025-11-17", "2026-11-16");
    undated[1] = "RB2611.SHF";
    undated[4] = std::nullopt;
    auto copper = listing("SHFE/cu/2026-04", "2025-04-16", "2026-04-15");
    copper[1] = "CU2604.SHF";
    const auto snapshot = keep(store, contracts, "all", {copper, rebar, undated}, 1000);
    const auto list =
        data::table_contracts(store, snapshot, "fixture.minutes", contracts.source, "SHFE", "rb");
    EXPECT_EQ(list.source(), "fixture.minutes");
    ASSERT_EQ(list.items_size(), 1);
    const auto& item = list.items(0);
    EXPECT_EQ(item.contract_id(), "SHFE/rb/2026-10");
    EXPECT_EQ(item.source_instrument(), "RB2610.SHF");
    EXPECT_EQ(item.name(), "螺纹钢2610");
    EXPECT_EQ(item.list_date(), "2025-10-16");
    EXPECT_EQ(item.delist_date(), "2026-10-15");
    EXPECT_FALSE(item.has_multiplier());
    EXPECT_EQ(item.per_unit().units(), Decimal::parse("10").raw());
    EXPECT_EQ(item.trade_unit(), "吨");
    EXPECT_EQ(item.quote_unit(), "元/吨");
    EXPECT_EQ(
        refusal([&] {
            (void)data::table_contracts(
                store,
                snapshot,
                "fixture.minutes",
                contracts.source,
                "DCE",
                "m");
        }),
        "no contract list of DCE is stored");
}

TEST(TableSeries, ChartCatalogUsesTheHeldContractListWithoutMinuteDownloadCodes)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    auto rebar = listing("SHFE/rb/2026-10", "2025-10-16", "2026-10-15");
    rebar[2] = "螺纹钢2610";
    auto undated = listing("SHFE/rb/2026-11", "2025-11-17", "2026-11-16");
    undated[4] = std::nullopt;
    const auto snapshot = keep(
        store,
        contracts,
        "all",
        {rebar, undated, listing("SHFE/cu/2026-04", "2025-04-16", "2026-04-15")},
        1000);
    // A later revision must not change a catalog selected from this snapshot.
    (void)keep(
        store,
        contracts,
        "all",
        {listing("SHFE/rb/2027-01", "2026-01-16", "2027-01-15")},
        2000);
    const Json selection{
        {"snapshot", snapshot},
        {"contracts_source", contracts.source},
        {"exchange", "SHFE"},
        {"product", "rb"}
    };
    const auto query = protocol::encode_table_listed_contracts_query(selection);
    EXPECT_EQ(
        protocol::decode_table_listed_contracts(data::table_listed_contracts(store, query)),
        (Json{
            {"query", selection},
            {"items",
                Json::array(
                    {{{"contract_id", "SHFE/rb/2026-10"},
                        {"name", "螺纹钢2610"},
                        {"list_date", "2025-10-16"},
                        {"delist_date", "2026-10-15"}}})}
        }));
    // These same rows cannot be used as provider download instructions.
    EXPECT_EQ(
        data::table_contracts(store, snapshot, "fixture.minutes", contracts.source, "SHFE", "rb")
            .items_size(),
        0);
    auto invalid = query;
    invalid.set_product("rb/2026-10");
    EXPECT_THROW((void)data::table_listed_contracts(store, invalid), std::invalid_argument);
}

TEST(TableSeries, AChartDrawsTheDaysAContractTradedFromItsListingOn)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    const auto snapshot = keep_chart(store);
    const auto all = data::table_daily(store, chart(snapshot, data::proto::DAY));
    // Forty-five days less the one without an auction trade; the Friday before
    // the listing is not read.
    ASSERT_EQ(all.total(), 44U);
    ASSERT_EQ(all.candles_size(), 44);
    const auto decoded = protocol::decode_table_daily(all);
    EXPECT_EQ(
        decoded.at("candles")[0],
        (Json{
            {"trading_day", "2026-01-05"},
            {"open", "3000"},
            {"high", "3010"},
            {"low", "2990"},
            {"close", "3005"},
            {"volume", "100"}
        }));
    EXPECT_EQ(all.candles(9).trading_day(), "2026-01-16");
    EXPECT_EQ(all.candles(10).trading_day(), "2026-01-20");
    // MACD warms up over the first 33 candles; a later page has the values the
    // whole run gives it.
    EXPECT_FALSE(all.candles(32).has_macd());
    EXPECT_TRUE(all.candles(33).has_macd());
    const auto last = data::table_daily(store, chart(snapshot, data::proto::DAY, 40, 10));
    ASSERT_EQ(last.candles_size(), 4);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(last.candles(i).SerializeAsString(), all.candles(40 + i).SerializeAsString());
    EXPECT_THROW(
        (void)data::table_daily(store, chart(snapshot, data::proto::DAY, 44)),
        std::invalid_argument);
}

TEST(TableSeries, AChartGroupsWeeksAndMonthsAndNeedsTheContractList)
{
    Folder folder("ast-ts-");
    TableStore store(folder.path / "tables");
    const auto snapshot = keep_chart(store);
    const auto weeks =
        protocol::decode_table_daily(data::table_daily(store, chart(snapshot, data::proto::WEEK)));
    EXPECT_EQ(weeks.at("total"), 9);
    // The week of 2026-01-19 opens on the Tuesday: the Monday has no candle.
    EXPECT_EQ(
        weeks.at("candles")[2],
        (Json{
            {"trading_day", "2026-01-23"},
            {"open", "3011"},
            {"high", "3024"},
            {"low", "3001"},
            {"close", "3019"},
            {"volume", "450"}
        }));
    const auto months =
        protocol::decode_table_daily(data::table_daily(store, chart(snapshot, data::proto::MONTH)));
    ASSERT_EQ(months.at("candles").size(), 3U);
    EXPECT_EQ(months.at("candles")[0].at("trading_day"), "2026-01-30");
    EXPECT_EQ(months.at("candles")[0].at("volume"), "2080");
    EXPECT_EQ(months.at("candles")[2].at("trading_day"), "2026-03-06");
    // Without the contract list a contract's days cannot be bounded.
    EXPECT_EQ(
        refusal([&] {
            (void)data::table_daily(
                store,
                chart(snapshot, data::proto::DAY, 0, 200, "SHFE/rb/2026-11"));
        }),
        "the contract list does not list SHFE/rb/2026-11");
    EXPECT_EQ(
        refusal([&] {
            (void)data::table_daily(
                store,
                chart(snapshot, data::proto::DAY, 0, 200, "DCE/m/2026-09"));
        }),
        "no contract list of DCE is stored");
}

// The real data service, task service and factor worker over a store that
// holds nine weeks of three products.
TEST(TableSeries, AFactorIsEvaluatedAcrossProductsOfTheTable)
{
    using namespace std::chrono_literals;
    Folder warehouse("ast-tf-"), tasks("ast-tt-"), sockets("ast-tk-");
    // The data service's own directory layout, then the tables as an update leaves them.
    {
        data::Store layout(warehouse.path, "table-data");
    }
    std::string snapshot;
    std::vector<std::string> open;
    {
        TableStore store(warehouse.path / "tables");
        std::vector<TableSourceRow> year;
        for (auto day = std::chrono::sys_days(parse_trading_date("2026-03-02")); open.size() < 45;
            day += std::chrono::days(1)) {
            const auto text = format_trading_date(std::chrono::year_month_day(day));
            const bool weekday = std::chrono::weekday(day).iso_encoding() < 6;
            year.push_back({text, weekday ? "1" : "0", std::nullopt});
            if (weekday)
                open.push_back(text);
        }
        (void)keep(store, calendar, "2026", year, 900);
        for (int d = 0; d < 45; ++d) {
            const auto price = [&](int value) {
                return std::to_string(value);
            };
            const auto rebar = price(3000 + 7 * d + d * d % 13),
                       copper = price(70000 - 11 * d + d * 37 % 29 * 10),
                       aluminium = price(20000 + d * 17 % 23 * 15 - 3 * d);
            // Rebar rolls from May to October in the fourth week.
            const auto near = std::to_string(d < 18 ? 500 : 100),
                       far = std::to_string(d < 18 ? 100 : 500);
            std::vector<TableSourceRow> daily_rows{
                traded("SHFE/rb/2026-05", rebar.c_str(), rebar, near),
                traded("SHFE/rb/2026-10", rebar.c_str(), rebar, far),
                traded("SHFE/cu/2026-06", copper.c_str(), copper, "300"),
                traded("SHFE/al/2026-06", aluminium.c_str(), aluminium, "300")
            };
            for (auto& row : daily_rows)
                row[8] = d == 8 && row[0]->starts_with("SHFE/rb/") ? "0" : "100";
            snapshot = keep(store, daily, open[static_cast<std::size_t>(d)], daily_rows, 1000 + d);
        }
    }
    const auto data_endpoint = (sockets.path / "data.sock").string(),
               data_workers = (sockets.path / "data.workers").string(),
               task_endpoint = (sockets.path / "task.sock").string();
    ChildProcess data_process(
        ASTERION_DATA_SERVICE_PATH,
        {"--directory",
            warehouse.path.string(),
            "--session",
            "table-data",
            "--node-id",
            "fixture-node",
            "--endpoint",
            data_endpoint,
            "--internal-endpoint",
            data_workers});
    testing_support::DataDiscovery discovery(
        task_endpoint + ".discovery",
        "table-data",
        data_endpoint);
    ChildProcess task_process(
        ASTERION_RESEARCH_PATH,
        {"--config",
            (std::filesystem::path(ASTERION_RESEARCH_PATH).parent_path() / "config/research.json")
                .string(),
            "--directory",
            tasks.path.string(),
            "--session",
            "table-tasks",
            "--endpoint",
            task_endpoint,
            "--discovery-endpoint",
            task_endpoint + ".discovery"});
    discovery.research("table-tasks", task_process.id(), sha256_file(ASTERION_RESEARCH_PATH));
    const auto call = [&](research::proto::ResearchRequest request) {
        request.set_version(1);
        request.set_service_id("table-tasks");
        request.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(task_endpoint, 2s);
        channel.send(request.SerializeAsString(), 2s);
        task::proto::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(10s)))
            throw std::runtime_error("bad response");
        return reply;
    };
    for (const auto deadline = std::chrono::steady_clock::now() + 10s;;) {
        try {
            research::proto::ResearchRequest ping;
            ping.mutable_heartbeat();
            data::proto::DataRequest data_ping;
            data_ping.mutable_heartbeat();
            if (call(ping).health().initialized()
                && protocol::DataClient(data_endpoint, "table-data")
                    .call(data_ping)
                    .health()
                    .initialized())
                break;
        } catch (const std::exception&) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        }
        std::this_thread::sleep_for(20ms);
    }
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("across");
    auto* factor = submit.mutable_submit()->mutable_factor_request();
    *factor->add_series()->mutable_tables() = request(
        snapshot,
        {"SHFE/rb", "SHFE/cu", "SHFE/al"},
        open.front().c_str(),
        open.back().c_str());
    factor->set_factor(protocol::proto::PRICE_MOMENTUM);
    factor->add_lookbacks(5);
    factor->set_horizon(1);
    factor->set_full_sample(true);
    factor->mutable_liquidity()->set_minimum_history(3);
    factor->mutable_liquidity()->set_minimum_volume(100);
    factor->mutable_liquidity()->set_minimum_open_interest(0);
    const auto submitted = call(submit);
    ASSERT_TRUE(submitted.has_task()) << submitted.DebugString();
    EXPECT_EQ(submitted.task().kind(), task::proto::FACTOR);
    EXPECT_EQ(submitted.task().instrument(), "SHFE/rb + SHFE/cu + SHFE/al");
    EXPECT_EQ(submitted.task().source_name(), snapshot) << "one snapshot, named once";
    // Each series begins on the second day: 44 shared observations.
    EXPECT_EQ(submitted.task().total(), 44U);
    // The worker is handed what to read, not the rows, and reads the same from Data.
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "across")
            .state(),
        task::proto::SUCCEEDED);
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("across");
    const auto result = call(read);
    ASSERT_EQ(result.result_task().state(), task::proto::SUCCEEDED) << result.DebugString();
    EXPECT_EQ(result.factor().input_count(), 44U);
    EXPECT_EQ(result.factor().cross_sections_size(), 38) << "44 less the window and the label";
    const auto& input = result.result_task().factor();
    ASSERT_EQ(input.series_size(), 3);
    EXPECT_EQ(input.liquidity().minimum_volume(), 100);
    EXPECT_EQ(result.factor().memberships_size(), 132);
    const auto dropped = std::ranges::find(
        result.factor().cross_sections(),
        8U,
        &factor::proto::FactorCrossSection::event_index);
    ASSERT_NE(dropped, result.factor().cross_sections().end());
    EXPECT_EQ(dropped->members_size(), 2U);
    EXPECT_EQ((dropped + 1)->members_size(), 3U);
    EXPECT_EQ(input.series(0).table().months_size(), 2) << "rebar rolled";
    EXPECT_EQ(input.series(0).table().schedule().rolls(1).trading_day(), open[19]);
    const auto decoded = protocol::decode_task_result(result, "across");
    EXPECT_EQ(decoded.at("experiment").at("series").at(1).at("kind"), "table");
    EXPECT_EQ(decoded.at("experiment").at("series").at(1).at("data").at("instrument"), "SHFE/cu");
    EXPECT_EQ(decoded.at("result").at("cross_sections").at(0).at("observed"), open[6])
        << "observations are shown by trading day";
    const auto original_input = sha256_file(tasks.path / "across/input.pb");
    const auto original_result = sha256_file(tasks.path / "across/results/1.pb");
    research::proto::ResearchRequest repeat;
    repeat.mutable_submit()->set_id("bad-reuse");
    *repeat.mutable_submit()->mutable_factor_request() = *factor;
    repeat.mutable_submit()->mutable_factor_request()->set_expected_dataset_revision(
        std::string(64, 'f'));
    const auto refused = call(repeat);
    ASSERT_TRUE(refused.has_error());
    EXPECT_EQ(refused.error().message(), "reused factor data does not match the original revision");
    EXPECT_FALSE(std::filesystem::exists(tasks.path / "bad-reuse/input.pb"));
    repeat.mutable_submit()->set_id("fixed-reuse");
    repeat.mutable_submit()->mutable_factor_request()->set_expected_dataset_revision(
        input.dataset_revision());
    ASSERT_TRUE(call(repeat).has_task());
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "fixed-reuse")
            .state(),
        task::proto::SUCCEEDED);
    read.mutable_result()->set_id("fixed-reuse");
    const auto repeated = call(read);
    ASSERT_EQ(repeated.result_task().state(), task::proto::SUCCEEDED);
    EXPECT_EQ(repeated.factor().SerializeAsString(), result.factor().SerializeAsString());
    EXPECT_EQ(sha256_file(tasks.path / "across/input.pb"), original_input);
    EXPECT_EQ(sha256_file(tasks.path / "across/results/1.pb"), original_result);
}

TEST(TableSeries, LiquidityLotsAreFixedUnknownIsNotZeroAndPricesDoNotScaleThem)
{
    Folder folder("ast-liquidity-");
    TableStore store(folder.path);
    keep_calendar(store);
    auto near = traded("SHFE/rb/2026-05", "100", "100", "500");
    auto far = traded("SHFE/rb/2026-10", "200", "200", "100");
    near[8] = "123";
    far[8] = "0";
    std::string snapshot;
    for (int i = 0; i < 4; ++i) {
        if (i == 2)
            near[10] = "50";
        snapshot = keep(store, daily, days[i], {near, far}, 1000 + i);
    }
    const auto query = request(snapshot, {"SHFE/rb"}, days[0], days[3]);
    const auto fixed = data::table_series(store, query).series(0);
    ASSERT_EQ(fixed.months_size(), 2);
    EXPECT_EQ(Decimal::from_raw(fixed.schedule().rolls(0).factor().units()), Decimal::parse("2"));
    const auto& row = fixed.months(0).closes(0);
    EXPECT_TRUE(row.has_volume_contracts());
    EXPECT_EQ(row.volume_contracts(), 123);
    EXPECT_EQ(row.open_interest_contracts(), 500);
    const auto revision = protocol::table_series_revision(fixed);
    near[8] = std::nullopt;
    (void)keep(store, daily, days[1], {near, far}, 2000);
    EXPECT_EQ(
        protocol::table_series_revision(data::table_series(store, query).series(0)),
        revision);
    auto latest = query;
    latest.set_snapshot(store.current(daily, "SHFE", days[1])->snapshot);
    const auto unknown = data::table_series(store, latest).series(0);
    EXPECT_FALSE(unknown.months(0).closes(0).has_volume_contracts());
    EXPECT_NE(protocol::table_series_revision(unknown), revision);
    const auto zero =
        data::table_series(store, request(snapshot, {"SHFE/rb/2026-10"}, days[0], days[2]))
            .series(0);
    EXPECT_TRUE(zero.months(0).closes(0).has_volume_contracts());
    EXPECT_EQ(zero.months(0).closes(0).volume_contracts(), 0);
}
