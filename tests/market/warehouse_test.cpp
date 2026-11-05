#include "warehouse/reader.hpp"
#include "warehouse/research.hpp"
#include "support/market_host.hpp"
#include "parquet_engine.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/market.pb.h>
#include <gtest/gtest.h>

namespace {

using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace wh = market_data::warehouse;
namespace wire = ::market;

class Warehouse : public ::testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("asterion-warehouse-" + unique_process_id());

    void TearDown() override
    {
        fail_next_directory_syncs_for_testing(0);
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

wh::Row row(std::string_view table)
{
    wh::Row result;
    for (const auto& column : wh::columns(table)) {
        switch (column.kind) {
            case wh::Kind::text:
                result.emplace_back(std::string("铜'合约"));
                break;
            case wh::Kind::date:
                result.emplace_back(std::string("2026-10-30"));
                break;
            case wh::Kind::boolean:
                result.emplace_back(true);
                break;
            case wh::Kind::integer:
                result.emplace_back(std::int64_t{1793318400000});
                break;
            case wh::Kind::number:
                result.emplace_back(std::string("1234567890123456789012.123456"));
                break;
        }
    }
    return result;
}

wh::Version publish(wh::Store& store, const std::string& key, std::size_t count = 1)
{
    auto writer = store.writer();
    const std::vector<wh::Row> rows(count, row(key.substr(0, key.find('/'))));
    writer.put(key, rows, {"2026-10-30"});
    return writer.commit();
}

TEST_F(Warehouse, ImmutableVersionsAndAbandonedWrites)
{
    wh::Store store(root, "tushare");
    EXPECT_TRUE(store.current().identity.empty());
    EXPECT_FALSE(fs::exists(root));
    const auto first = publish(store, "daily/SHFE/202610");
    const auto first_file = store.file(first, "daily/SHFE/202610");
    {
        auto abandoned = store.writer();
        abandoned.put("calendar/SHFE/all", std::vector<wh::Row>{row("calendar")});
    }
    EXPECT_EQ(store.current().identity, first.identity);
    EXPECT_EQ(store.current().partitions.size(), 1);
    const auto second = publish(store, "daily/SHFE/202610", 2);
    EXPECT_NE(first.identity, second.identity);
    EXPECT_EQ(second.parent, first.identity);
    EXPECT_EQ(store.version(first.identity).partitions.at("daily/SHFE/202610").rows, 1);
    EXPECT_EQ(sha256_file(first_file), first.partitions.at("daily/SHFE/202610").sha256);
    EXPECT_EQ(store.current().partitions.at("daily/SHFE/202610").rows, 2);
    EXPECT_EQ(publish(store, "daily/SHFE/202610", 2).identity, second.identity);
    EXPECT_THROW(store.version(std::string(64, '0')), Error);
    EXPECT_THROW(store.version("../current"), std::invalid_argument);
    {
        auto writer = store.writer();
        writer.put("calendar/SHFE/all", std::vector<wh::Row>{row("calendar")});
        fail_next_directory_syncs_for_testing(1);
        EXPECT_THROW(writer.commit(), std::runtime_error);
    }
    EXPECT_EQ(store.current().identity, second.identity);
}

TEST_F(Warehouse, ACommitWritesOnlyTheMonthsItChanged)
{
    wh::Store store(root, "recorder");
    const auto shards = [&] {
        return std::distance(
            fs::directory_iterator(store.root() / "shards"),
            fs::directory_iterator{});
    };
    publish(store, "minutes/SHFE/2026-10-28/rb/2027-01");
    const auto first = publish(store, "minutes/DCE/2026-10-28/i/2027-01");
    EXPECT_EQ(first.partitions.size(), 2);
    const auto written = shards();
    // A new SHFE day rewrites the SHFE month only; DCE's shard is shared.
    const auto second = publish(store, "minutes/SHFE/2026-10-29/rb/2027-01");
    EXPECT_EQ(shards(), written + 1);
    EXPECT_EQ(second.partitions.size(), 3);
    EXPECT_EQ(store.version(first.identity).partitions.size(), 2);
    EXPECT_LT(fs::file_size(store.root() / "versions" / (second.identity + ".json")), 1024);
}

TEST_F(Warehouse, EveryTablePreservesTypesPrecisionNullsAndText)
{
    wh::Store store(root, "tushare");
    auto writer = store.writer();
    for (const auto* table :
        {"daily", "calendar", "contracts", "minutes", "settlement", "observations"}) {
        const std::vector<wh::Row> rows{row(table), wh::Row(wh::columns(table).size())};
        writer.put(std::string(table) + "/SHFE/all", rows);
    }
    const auto version = writer.commit();
    for (const auto& [key, partition] : version.partitions) {
        const auto schema = wh::columns(key.substr(0, key.find('/')));
        parquet::Engine engine;
        const auto data = engine.run(
            "SELECT * FROM read_parquet(" + parquet::literal(store.file(version, key)) + ")");
        ASSERT_EQ(data->RowCount(), 2);
        ASSERT_EQ(data->ColumnCount(), schema.size());
        for (std::size_t c = 0; c < schema.size(); ++c) {
            EXPECT_EQ(data->names[c], schema[c].name);
            EXPECT_TRUE(data->GetValue(c, 1).IsNull());
            const auto cell = data->GetValue(c, 0);
            switch (schema[c].kind) {
                case wh::Kind::text:
                    EXPECT_EQ(cell.ToString(), "铜'合约");
                    break;
                case wh::Kind::date:
                    EXPECT_EQ(cell.ToString(), "2026-10-30");
                    break;
                case wh::Kind::boolean:
                    EXPECT_TRUE(cell.GetValue<bool>());
                    break;
                case wh::Kind::integer:
                    EXPECT_EQ(cell.GetValue<std::int64_t>(), 1793318400000);
                    break;
                case wh::Kind::number:
                    EXPECT_EQ(cell.type(), duckdb::LogicalType::DECIMAL(28, 6));
                    EXPECT_EQ(cell.ToString(), "1234567890123456789012.123456");
                    break;
            }
        }
    }
}

TEST_F(Warehouse, RefusesLossyInputAndDoesNotPublishIt)
{
    wh::Store store(root, "tushare");
    auto writer = store.writer();
    for (const auto* invalid : {"1.0000001", "10000000000000000000000", "nan", "1e3"}) {
        auto data = row("daily");
        data[4] = std::string(invalid);
        EXPECT_THROW(
            writer.put("daily/SHFE/202610", std::vector<wh::Row>{data}),
            std::invalid_argument);
    }
    for (const auto* date : {"2026-02-30", "0000-01-01"}) {
        auto bad_day = row("daily");
        bad_day[3] = std::string(date);
        EXPECT_THROW(
            writer.put("daily/SHFE/202610", std::vector<wh::Row>{bad_day}),
            std::invalid_argument);
    }
    EXPECT_THROW(writer.put("daily/../escape", {}), std::invalid_argument);
    EXPECT_TRUE(writer.commit().identity.empty());
    EXPECT_FALSE(fs::exists(store.root() / "current"));
    auto data = row("daily");
    data[4] = Decimal::parse("-12.125");
    data[5] = std::string("1.00000000");
    writer.put("daily/SHFE/202610", std::vector<wh::Row>{data});
    const auto version = writer.commit();
    parquet::Engine engine;
    const auto values = engine.run(
        "SELECT open, high FROM read_parquet("
        + parquet::literal(store.file(version, "daily/SHFE/202610")) + ")");
    EXPECT_EQ(values->GetValue(0, 0).ToString(), "-12.125000");
    EXPECT_EQ(values->GetValue(1, 0).ToString(), "1.000000");
}

TEST_F(Warehouse, ResearchTakesTushareFirstAndRecordedMinutesOnlyWhereTushareHasNone)
{
    const auto store = [&](const char* provider) {
        return std::make_unique<wh::Store>(wh::source_directory(root, provider), provider);
    };
    const std::string mine = "minutes/SHFE/2026-10-29/rb/2027-01";
    const std::string recorded_only = "minutes/SHFE/2026-10-30/rb/2027-01";
    auto tushare = store("tushare"), shfe = store("shfe"), recorder = store("recorder");
    publish(*tushare, "daily/SHFE/2026-10");
    publish(*tushare, "contracts/SHFE");
    publish(*tushare, mine, 2);
    publish(*shfe, "settlement/SHFE/2026-10-30");
    publish(*shfe, "daily/SHFE/2026-10-30");
    publish(*recorder, mine, 3);
    publish(*recorder, recorded_only, 3);
    publish(*recorder, "observations/SHFE/2026-10-30/rb/2027-01");
    wh::Store composed(wh::source_directory(root, "research"), "research");
    const auto research = wh::compose_research(root, composed);
    std::map<std::string, std::string> sources;
    for (const auto& [key, part] : research.partitions)
        sources[key] = part.source;
    EXPECT_EQ(
        sources,
        (std::map<std::string, std::string>{
            {"contracts/SHFE", "tushare"},
            {"daily/SHFE/2026-10", "tushare"},
            {mine, "tushare"},
            {recorded_only, "recorder"},
            {"observations/SHFE/2026-10-30/rb/2027-01", "recorder"},
            {"settlement/SHFE/2026-10-30", "shfe"}
        }));
    // Files are linked, not copied.
    const wh::Store reader(wh::source_directory(root, "research"), "research");
    EXPECT_TRUE(
        fs::equivalent(
            reader.file(research, recorded_only),
            recorder->file(recorder->current(), recorded_only)));
    EXPECT_EQ(reader.read(research, mine).size(), 2U);
    EXPECT_EQ(wh::compose_research(root, composed).identity, research.identity);
    // Once Tushare has the recorded day, research takes it from Tushare alone.
    publish(*tushare, recorded_only, 4);
    const auto later = wh::compose_research(root, composed);
    EXPECT_EQ(later.partitions.at(recorded_only).source, "tushare");
    EXPECT_EQ(reader.read(later, recorded_only).size(), 4U);
    EXPECT_FALSE(later.partitions.contains("observations/SHFE/2026-10-30/rb/2027-01"));
    EXPECT_EQ(reader.read(research, recorded_only).size(), 3U) << "earlier versions are unchanged";
}

TEST_F(Warehouse, MinuteCoverageCountsDaysPerContractAndMonthAndMarksRecordedDays)
{
    const auto store = [&](const char* provider) {
        return std::make_unique<wh::Store>(wh::source_directory(root, provider), provider);
    };
    auto tushare = store("tushare"), recorder = store("recorder");
    publish(*tushare, "minutes/SHFE/2026-09-30/rb/2027-01", 2);
    publish(*tushare, "minutes/SHFE/2026-10-29/rb/2027-01", 2);
    publish(*recorder, "minutes/SHFE/2026-10-30/rb/2027-01", 2);
    publish(*recorder, "minutes/SHFE/2026-10-30/cu/2026-12", 2);
    wh::Store composed(wh::source_directory(root, "research"), "research");
    wire::DatasetRef query;
    query.set_provider("research");
    query.set_version(wh::compose_research(root, composed).identity);
    const auto coverage = wh::Reader(root).coverage(query);
    ASSERT_EQ(coverage.contracts_size(), 2);
    EXPECT_EQ(coverage.contracts(0).contract(), "SHFE/cu/2026-12");
    EXPECT_EQ(coverage.contracts(0).months(0).recorded_days(), 1);
    const auto& rb = coverage.contracts(1);
    EXPECT_EQ(rb.contract(), "SHFE/rb/2027-01");
    ASSERT_EQ(rb.months_size(), 2);
    EXPECT_EQ(rb.months(0).month(), "2026-09");
    EXPECT_EQ(rb.months(0).days(), 1);
    EXPECT_EQ(rb.months(0).recorded_days(), 0);
    EXPECT_EQ(rb.months(1).month(), "2026-10");
    EXPECT_EQ(rb.months(1).days(), 2);
    EXPECT_EQ(rb.months(1).recorded_days(), 1);
}

TEST_F(Warehouse, VersionsListNewestFirstWithWhatEachAddedReplacedAndRemoved)
{
    wh::Store store(root, "tushare");
    const auto commit = [&](bool clear, std::vector<std::pair<std::string, std::size_t>> parts) {
        auto writer = store.writer();
        if (clear)
            writer.clear();
        for (const auto& [key, count] : parts)
            writer.put(key, std::vector<wh::Row>(count, row("daily")));
        return writer.commit().identity;
    };
    const auto first = commit(false, {{"daily/SHFE/2026-09", 1}, {"daily/SHFE/2026-10", 1}});
    const auto second = commit(false, {{"daily/SHFE/2026-10", 2}, {"daily/DCE/2026-10", 1}});
    const auto third = commit(true, {{"daily/SHFE/2026-09", 1}});
    wh::Reader reader(root);
    wire::ListDatasetVersions query;
    query.set_provider("tushare");
    const auto listed = reader.versions(query);
    ASSERT_EQ(listed.versions_size(), 3);
    EXPECT_TRUE(listed.next_version().empty());
    const auto& newest = listed.versions(0);
    EXPECT_EQ(newest.dataset().version(), third);
    EXPECT_EQ(newest.base(), second);
    EXPECT_EQ(newest.removed(), 2);
    const auto& middle = listed.versions(1);
    EXPECT_EQ(middle.added(), 1);
    EXPECT_EQ(middle.replaced(), 1);
    EXPECT_EQ(middle.removed(), 0);
    EXPECT_EQ(listed.versions(2).added(), 2);
    EXPECT_TRUE(listed.versions(2).base().empty());
    query.set_after_version(third);
    EXPECT_EQ(reader.versions(query).versions(0).dataset().version(), second);
    wire::CompareDatasetVersions compare;
    compare.set_provider("tushare");
    compare.set_version(second);
    compare.set_base(first);
    const auto change = reader.change(compare);
    EXPECT_EQ(change.added(), 1);
    EXPECT_EQ(change.replaced(), 1);
    EXPECT_EQ(change.removed(), 0);
}

TEST_F(Warehouse, ReferenceTablesPageByExchangeAndDays)
{
    wh::Store store(root, "tushare");
    const auto with = [](const char* table, std::size_t column, wh::Cell value) {
        auto out = row(table);
        out[column] = std::move(value);
        return out;
    };
    {
        auto writer = store.writer();
        writer.put(
            "contracts/SHFE",
            std::vector<wh::Row>{
                with("contracts", 1, std::string("SHFE/rb/2027-01")),
                with("contracts", 1, std::string("SHFE/cu/2026-12"))
            });
        writer.put("contracts/DCE", std::vector<wh::Row>{with("contracts", 0, std::string("DCE"))});
        writer.put(
            "calendar/SHFE/2026",
            std::vector<wh::Row>{
                with("calendar", 1, std::string("2026-10-29")),
                with("calendar", 1, std::string("2026-10-30"))
            });
        writer.commit();
    }
    wh::Reader reader(root);
    wire::GetTableRows query;
    query.mutable_dataset()->set_provider("tushare");
    query.set_table(wire::TABLE_CONTRACTS);
    query.set_exchange("SHFE");
    const auto contracts = reader.rows(query);
    EXPECT_EQ(contracts.columns(1), "contract");
    ASSERT_EQ(contracts.total(), 2);
    EXPECT_EQ(contracts.rows(0).values(1), "SHFE/cu/2026-12");
    query.set_table(wire::TABLE_CALENDAR);
    query.set_first_day("2026-10-30");
    query.set_last_day("2026-12-31");
    const auto calendar = reader.rows(query);
    ASSERT_EQ(calendar.total(), 1);
    EXPECT_EQ(calendar.rows(0).values(1), "2026-10-30");
    query.set_table(wire::TABLE_MINUTES);
    EXPECT_THROW(reader.rows(query), std::invalid_argument);
}

TEST_F(Warehouse, MinuteComparisonCountsDifferencesPartialAndMissingMinutes)
{
    const auto minute = [](std::int64_t start, const char* close, bool partial) {
        return wh::Row{
            std::string("SHFE"),
            std::string("rb"),
            std::string("SHFE/rb/2027-01"),
            std::string("2026-10-30"),
            start,
            partial,
            std::string("3100"),
            std::string("3110"),
            std::string("3090"),
            std::string(close),
            std::string("12"),
            wh::Cell{},
            std::string("2000")
        };
    };
    const std::string key = "minutes/SHFE/2026-10-30/rb/2027-01";
    const auto put = [&](const char* provider, std::vector<wh::Row> rows) {
        wh::Store store(wh::source_directory(root, provider), provider);
        auto writer = store.writer();
        writer.put(key, rows, {"2026-10-30"});
        writer.commit();
    };
    constexpr std::int64_t open = 1793322000000;
    put("tushare",
        {minute(open, "3100", false),
            minute(open + 60000, "3105", false),
            minute(open + 120000, "3101", false),
            minute(open + 180000, "3102", false)});
    put("recorder",
        {minute(open, "3100", false),
            minute(open + 60000, "3106", false),
            minute(open + 120000, "3101", true)});
    const auto comparison = wh::compare_minutes(root);
    EXPECT_EQ(comparison.days(), 1U);
    EXPECT_EQ(comparison.differing_days(), 1U);
    ASSERT_EQ(comparison.differing_size(), 1);
    const auto& day = comparison.differing(0);
    EXPECT_EQ(day.contract(), "SHFE/rb/2027-01");
    EXPECT_EQ(day.trading_day(), "2026-10-30");
    EXPECT_EQ(day.compared(), 2U);
    EXPECT_EQ(day.differing(), 1U);
    EXPECT_EQ(day.tushare_only(), 1U);
    EXPECT_EQ(day.recorder_only(), 0U);
    EXPECT_EQ(day.partial(), 1U);
    ::market::GetMinuteDifferences query;
    query.set_contract("SHFE/rb/2027-01");
    query.set_trading_day("2026-10-30");
    const auto differences = wh::minute_differences(root, query);
    ASSERT_EQ(differences.minutes_size(), 2);
    EXPECT_EQ(differences.minutes(0).start_ms(), open + 60000);
    EXPECT_EQ(differences.minutes(0).tushare().close().text(), "3105");
    EXPECT_EQ(differences.minutes(0).recorder().close().text(), "3106");
    EXPECT_EQ(differences.minutes(0).recorder().volume(), 12);
    EXPECT_EQ(differences.minutes(1).start_ms(), open + 180000);
    EXPECT_TRUE(differences.minutes(1).has_tushare());
    EXPECT_FALSE(differences.minutes(1).has_recorder());
    query.set_trading_day("../../etc");
    EXPECT_THROW(wh::minute_differences(root, query), std::exception);
}

TEST_F(Warehouse, PagedMetadataAndChunkedFilesStayOnSelectedVersion)
{
    wh::Store store(root, "tushare");
    publish(store, "daily/SHFE/202610");
    const auto old = publish(store, "calendar/SHFE/all");
    wh::Reader reader(root, 1);
    wire::GetDatasetVersion query;
    query.mutable_dataset()->set_provider("tushare");
    const auto page = reader.version(query);
    ASSERT_EQ(page.partitions_size(), 1);
    EXPECT_EQ(page.dataset().version(), old.identity);
    ASSERT_FALSE(page.next_key().empty());
    publish(store, "daily/SHFE/202610", 3);
    query.set_after_key(page.next_key());
    EXPECT_THROW(reader.version(query), std::invalid_argument);
    query.mutable_dataset()->set_version(page.dataset().version());
    const auto next = reader.version(query);
    ASSERT_EQ(next.partitions_size(), 1);
    EXPECT_EQ(next.partitions(0).rows(), 1);
    EXPECT_EQ(next.partitions(0).table(), wire::TABLE_DAILY);
    EXPECT_TRUE(next.next_key().empty());
    query.clear_after_key();
    query.set_table(wire::TABLE_DAILY);
    query.set_exchange("DCE");
    EXPECT_EQ(reader.version(query).partitions_size(), 0);

    wire::ReadDatasetFile file;
    file.mutable_dataset()->set_provider("tushare");
    file.mutable_dataset()->set_version(old.identity);
    file.set_key("daily/SHFE/202610");
    const auto whole = reader.file(file);
    EXPECT_EQ(whole.size(), whole.data().size());
    EXPECT_EQ(sha256_bytes(whole.data()), old.partitions.at(file.key()).sha256);
    EXPECT_EQ(whole.sha256(), old.partitions.at(file.key()).sha256);
    file.set_offset(10);
    EXPECT_EQ(reader.file(file).data(), whole.data().substr(10));
    file.set_offset(whole.size());
    EXPECT_TRUE(reader.file(file).data().empty());
    file.set_offset(whole.size() + 1);
    EXPECT_THROW(reader.file(file), std::invalid_argument);
    file.set_offset(0);
    file.set_key("daily/../current");
    EXPECT_THROW(reader.file(file), std::invalid_argument);
    file.set_key("daily/DCE/missing");
    EXPECT_THROW(reader.file(file), Error);
    file.mutable_dataset()->clear_version();
    EXPECT_THROW(reader.file(file), std::invalid_argument);
}

TEST_F(Warehouse, SourcesAndSummaryAreReadOnlyAndVersioned)
{
    wh::Reader reader(root);
    EXPECT_EQ(reader.datasets().datasets_size(), 0);
    EXPECT_FALSE(fs::exists(root));
    wh::Store first(root, "tushare");
    const auto version = publish(first, "daily/SHFE/202610", 2);
    wh::Store other(wh::source_directory(root, "exchange"), "exchange");
    publish(other, "settlement/SHFE/202610");
    const auto datasets = reader.datasets();
    ASSERT_EQ(datasets.datasets_size(), 2);
    EXPECT_EQ(datasets.datasets(0).provider(), "exchange");
    EXPECT_EQ(datasets.datasets(1).version(), version.identity);
    wire::DatasetRef summary;
    summary.set_provider("tushare");
    summary.set_version(version.identity);
    publish(first, "daily/SHFE/202610", 3);
    const auto coverage = reader.summary(summary);
    ASSERT_EQ(coverage.tables_size(), 6);
    EXPECT_EQ(coverage.tables(0).table(), wire::TABLE_DAILY);
    EXPECT_EQ(coverage.tables(0).rows(), 2);
    EXPECT_EQ(coverage.tables(0).first_day(), "2026-10-30");
    EXPECT_EQ(coverage.tables(0).last_day(), "2026-10-30");
    EXPECT_EQ(coverage.tables(1).rows(), 0);
    EXPECT_TRUE(coverage.tables(1).first_day().empty());
}

TEST_F(Warehouse, ProductCoverageStaysOnOneVersionAndMissingCalendarStaysUnknown)
{
    wh::Store store(root, "tushare");
    const auto daily = [](const std::string& product, const std::string& day) {
        auto value = row("daily");
        value[0] = std::string("SHFE");
        value[1] = product;
        value[2] = "SHFE/" + product + "/2027-01";
        value[3] = day;
        return value;
    };
    wh::Version first;
    {
        auto writer = store.writer();
        writer.put(
            "daily/SHFE/2026-10",
            std::vector<wh::Row>{
                daily("cu", "2026-10-29"),
                daily("cu", "2026-10-30"),
                daily("rb", "2026-10-30")
            });
        writer.put("daily/SHFE/2026-09", std::vector<wh::Row>{daily("cu", "2026-09-30")});
        auto calendar = row("calendar");
        calendar[0] = std::string("SHFE");
        writer.put("calendar/SHFE/2026", std::vector<wh::Row>{calendar});
        first = writer.commit();
    }
    wh::Reader reader(root);
    wire::DatasetRef query;
    query.set_provider("tushare");
    EXPECT_THROW(reader.coverage(query), std::invalid_argument);
    query.set_version(first.identity);
    const auto coverage = reader.coverage(query);
    ASSERT_EQ(coverage.products_size(), 2);
    const auto& product = coverage.products(0);
    EXPECT_EQ(product.product(), "SHFE/cu");
    ASSERT_EQ(product.months_size(), 2);
    EXPECT_EQ(product.months(0).month(), "2026-09");
    EXPECT_EQ(product.months(0).days(), 1);
    EXPECT_EQ(product.months(0).open_days(), 0);
    EXPECT_EQ(product.months(1).days(), 2);
    EXPECT_EQ(product.months(1).open_days(), 1);
    EXPECT_EQ(coverage.products(1).product(), "SHFE/rb");
    wh::Version current;
    {
        auto writer = store.writer();
        writer.put("daily/SHFE/2026-10", std::vector<wh::Row>{daily("al", "2026-10-30")});
        current = writer.commit();
    }
    // A fixed version is not affected by a later commit.
    EXPECT_EQ(reader.coverage(query).products_size(), 2);
    query.set_version(current.identity);
    EXPECT_EQ(reader.coverage(query).products(0).product(), "SHFE/al");
    wh::Store public_source(wh::source_directory(root, "shfe"), "shfe");
    {
        auto writer = public_source.writer();
        writer.put("daily/SHFE/2026-10", std::vector<wh::Row>{daily("cu", "2026-10-30")});
        query.set_version(writer.commit().identity);
    }
    query.set_provider("shfe");
    EXPECT_EQ(reader.coverage(query).products(0).months(0).open_days(), 0);
    query.set_version(std::string(64, '0'));
    EXPECT_THROW(reader.coverage(query), Error);
}

::market::Response call(testing_support::MarketChannel& channel, ::market::Request request)
{
    request.set_id(1);
    channel.send(request.SerializeAsString(), 5s);
    ::market::Response response;
    if (!response.ParseFromString(channel.receive(5s)))
        throw std::runtime_error("invalid Market response");
    return response;
}

TEST_F(Warehouse, RemoteClientsKeepIndependentVersionsAndReconnect)
{
    testing_support::MarketHost host(root);
    wh::Store store(root / "data", "tushare");
    const auto old = publish(store, "daily/SHFE/202610");
    host.start();
    auto first = host.open();
    auto second = host.open();
    ::market::Request request;
    request.mutable_get_dataset_version()->mutable_dataset()->set_provider("tushare");
    const auto baseline = call(first, request);
    ASSERT_TRUE(baseline.has_get_dataset_version()) << baseline.DebugString();
    EXPECT_EQ(baseline.get_dataset_version().dataset().version(), old.identity);
    const auto latest = publish(store, "daily/SHFE/202610", 2);
    EXPECT_EQ(call(second, request).get_dataset_version().dataset().version(), latest.identity);
    request.mutable_get_dataset_version()->mutable_dataset()->set_version(old.identity);
    EXPECT_EQ(call(first, request).get_dataset_version().partitions(0).rows(), 1);
    request.mutable_get_dataset_version()->mutable_dataset()->set_version(std::string(64, '0'));
    EXPECT_TRUE(call(first, request).has_error());
    request.mutable_get_status();
    EXPECT_TRUE(call(second, request).has_get_status());
    first = host.open();
    auto* file = request.mutable_read_dataset_file();
    file->mutable_dataset()->set_provider("tushare");
    file->mutable_dataset()->set_version(old.identity);
    file->set_key("daily/SHFE/202610");
    const auto data = call(first, request);
    ASSERT_TRUE(data.has_read_dataset_file()) << data.DebugString();
    EXPECT_EQ(
        sha256_bytes(data.read_dataset_file().data()),
        old.partitions.at("daily/SHFE/202610").sha256);
    request.mutable_get_status();
    EXPECT_TRUE(call(first, request).has_get_status());
}

} // namespace
