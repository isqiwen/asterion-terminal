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
namespace wire = dataset::proto;

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
    EXPECT_EQ(day.day(), "2026-10-30");
    EXPECT_EQ(day.compared(), 2U);
    EXPECT_EQ(day.differing(), 1U);
    EXPECT_EQ(day.tushare_only(), 1U);
    EXPECT_EQ(day.recorder_only(), 0U);
    EXPECT_EQ(day.partial(), 1U);
    dataset::proto::DifferencesQuery query;
    query.set_contract("SHFE/rb/2027-01");
    query.set_day("2026-10-30");
    const auto differences = wh::minute_differences(root, query);
    ASSERT_EQ(differences.minutes_size(), 2);
    EXPECT_EQ(differences.minutes(0).start_ms(), open + 60000);
    EXPECT_EQ(differences.minutes(0).tushare().close(), "3105");
    EXPECT_EQ(differences.minutes(0).recorder().close(), "3106");
    EXPECT_EQ(differences.minutes(0).recorder().volume(), "12");
    EXPECT_EQ(differences.minutes(1).start_ms(), open + 180000);
    EXPECT_TRUE(differences.minutes(1).has_tushare());
    EXPECT_FALSE(differences.minutes(1).has_recorder());
    query.set_day("../../etc");
    EXPECT_THROW(wh::minute_differences(root, query), std::exception);
}

TEST_F(Warehouse, PagedMetadataAndChunkedFilesStayOnSelectedVersion)
{
    wh::Store store(root, "tushare");
    publish(store, "daily/SHFE/202610");
    const auto old = publish(store, "calendar/SHFE/all");
    wh::Reader reader(root);
    wire::VersionQuery query;
    query.set_provider("tushare");
    query.set_limit(1);
    const auto page = reader.version(query);
    ASSERT_EQ(page.partitions_size(), 1);
    EXPECT_EQ(page.dataset_version(), old.identity);
    ASSERT_FALSE(page.next_key().empty());
    publish(store, "daily/SHFE/202610", 3);
    query.set_after_key(page.next_key());
    EXPECT_THROW(reader.version(query), std::invalid_argument);
    query.set_dataset_version(page.dataset_version());
    const auto next = reader.version(query);
    ASSERT_EQ(next.partitions_size(), 1);
    EXPECT_EQ(next.partitions(0).rows(), 1);
    EXPECT_TRUE(next.next_key().empty());
    query.clear_after_key();
    query.set_table("daily");
    query.set_exchange("DCE");
    EXPECT_EQ(reader.version(query).partitions_size(), 0);
    query.set_limit(513);
    EXPECT_THROW(reader.version(query), std::invalid_argument);

    wire::FileQuery file;
    file.set_provider("tushare");
    file.set_dataset_version(old.identity);
    file.set_key("daily/SHFE/202610");
    file.set_limit(137);
    std::string bytes;
    std::uint64_t size = 0;
    do {
        const auto chunk = reader.file(file);
        EXPECT_EQ(chunk.offset(), bytes.size());
        EXPECT_EQ(chunk.dataset_version(), old.identity);
        bytes += chunk.data();
        size = chunk.size();
        file.set_offset(bytes.size());
    } while (bytes.size() < size);
    EXPECT_EQ(sha256_bytes(bytes), old.partitions.at(file.key()).sha256);
    EXPECT_TRUE(reader.file(file).data().empty());
    file.set_offset(size + 1);
    EXPECT_THROW(reader.file(file), std::invalid_argument);
    file.set_offset(0);
    file.set_key("daily/../current");
    EXPECT_THROW(reader.file(file), std::invalid_argument);
    file.set_key("daily/DCE/missing");
    EXPECT_THROW(reader.file(file), Error);
    file.clear_dataset_version();
    EXPECT_THROW(reader.file(file), std::invalid_argument);
}

TEST_F(Warehouse, SourcesAndSummaryAreReadOnlyAndVersioned)
{
    wh::Reader reader(root);
    EXPECT_EQ(reader.sources({}).sources_size(), 0);
    EXPECT_FALSE(fs::exists(root));
    wh::Store first(root, "tushare");
    const auto version = publish(first, "daily/SHFE/202610", 2);
    wh::Store other(wh::source_directory(root, "exchange"), "exchange");
    publish(other, "settlement/SHFE/202610");
    wire::SourcesQuery query;
    query.set_limit(1);
    const auto sources = reader.sources(query);
    ASSERT_EQ(sources.sources_size(), 1);
    EXPECT_EQ(sources.sources(0).provider(), "exchange");
    query.set_after_provider(sources.next_provider());
    const auto next = reader.sources(query);
    ASSERT_EQ(next.sources_size(), 1);
    EXPECT_EQ(next.sources(0).dataset_version(), version.identity);
    wire::SummaryQuery summary;
    summary.set_provider("tushare");
    summary.set_dataset_version(version.identity);
    publish(first, "daily/SHFE/202610", 3);
    const auto coverage = reader.summary(summary);
    ASSERT_EQ(coverage.tables_size(), 6);
    EXPECT_EQ(coverage.tables(0).rows(), 2);
    EXPECT_EQ(coverage.tables(0).first_day(), "2026-10-30");
    EXPECT_EQ(coverage.tables(0).last_day(), "2026-10-30");
    EXPECT_EQ(coverage.tables(1).rows(), 0);
    EXPECT_TRUE(coverage.tables(1).first_day().empty());
}

TEST_F(Warehouse, ProductCoveragePagesStayOnOneVersionAndMissingCalendarStaysUnknown)
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
    wire::CoverageQuery query;
    query.set_provider("tushare");
    EXPECT_THROW(reader.coverage(query), std::invalid_argument);
    query.set_dataset_version(first.identity);
    query.set_limit(1);
    const auto page = reader.coverage(query);
    ASSERT_EQ(page.products_size(), 1);
    EXPECT_EQ(page.next_product(), "SHFE/cu");
    const auto& product = page.products(0);
    ASSERT_EQ(product.months_size(), 2);
    EXPECT_EQ(product.months(0).month(), "2026-09");
    EXPECT_EQ(product.months(0).days(), 1);
    EXPECT_EQ(product.months(0).open_days(), 0);
    EXPECT_EQ(product.months(1).days(), 2);
    EXPECT_EQ(product.months(1).open_days(), 1);
    wh::Version current;
    {
        auto writer = store.writer();
        writer.put("daily/SHFE/2026-10", std::vector<wh::Row>{daily("al", "2026-10-30")});
        current = writer.commit();
    }
    query.set_after_product(page.next_product());
    const auto second = reader.coverage(query);
    ASSERT_EQ(second.products_size(), 1);
    EXPECT_EQ(second.products(0).product(), "rb");
    EXPECT_EQ(second.dataset_version(), first.identity);
    EXPECT_TRUE(second.next_product().empty());
    query.clear_after_product();
    query.set_dataset_version(current.identity);
    EXPECT_EQ(reader.coverage(query).products(0).product(), "al");
    wh::Store public_source(wh::source_directory(root, "shfe"), "shfe");
    {
        auto writer = public_source.writer();
        writer.put("daily/SHFE/2026-10", std::vector<wh::Row>{daily("cu", "2026-10-30")});
        query.set_dataset_version(writer.commit().identity);
    }
    query.set_provider("shfe");
    EXPECT_EQ(reader.coverage(query).products(0).months(0).open_days(), 0);
    query.set_limit(129);
    EXPECT_THROW(reader.coverage(query), std::invalid_argument);
    query.set_limit(1);
    query.set_dataset_version(std::string(64, '0'));
    EXPECT_THROW(reader.coverage(query), Error);
    query.set_after_product("SHFE/cu' OR TRUE");
    EXPECT_THROW(reader.coverage(query), std::invalid_argument);
}

market::proto::Response
call(testing_support::MarketChannel& channel, market::proto::Request request)
{
    request.set_version(1);
    request.set_service_id("market-data");
    request.set_correlation_id("warehouse-test");
    channel.send(request.SerializeAsString(), 5s);
    market::proto::Response response;
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
    market::proto::Request request;
    request.mutable_dataset()->set_provider("tushare");
    const auto baseline = call(first, request);
    ASSERT_TRUE(baseline.has_dataset()) << baseline.DebugString();
    EXPECT_EQ(baseline.dataset().dataset_version(), old.identity);
    const auto latest = publish(store, "daily/SHFE/202610", 2);
    EXPECT_EQ(call(second, request).dataset().dataset_version(), latest.identity);
    request.mutable_dataset()->set_dataset_version(old.identity);
    EXPECT_EQ(call(first, request).dataset().partitions(0).rows(), 1);
    request.mutable_dataset()->set_dataset_version(std::string(64, '0'));
    EXPECT_TRUE(call(first, request).has_error());
    request.mutable_heartbeat();
    EXPECT_TRUE(call(second, request).has_health());
    first = host.open();
    request.mutable_dataset_file()->set_provider("tushare");
    request.mutable_dataset_file()->set_dataset_version(old.identity);
    request.mutable_dataset_file()->set_key("daily/SHFE/202610");
    const auto data = call(first, request);
    ASSERT_TRUE(data.has_dataset_file()) << data.DebugString();
    EXPECT_EQ(
        sha256_bytes(data.dataset_file().data()),
        old.partitions.at("daily/SHFE/202610").sha256);
    request.mutable_heartbeat();
    EXPECT_TRUE(call(first, request).has_health());
}

} // namespace
