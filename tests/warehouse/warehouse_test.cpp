#include "reader.hpp"
#include "freshness.hpp"
#include "composition.hpp"
#include "gc.hpp"
#include "support/warehouse_host.hpp"
#include "parquet_engine.hpp"
#include <asterion/base/error.hpp>
#include <asterion/futures/trading_days.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/warehouse.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/warehouse.pb.h>
#include <fstream>
#include <gtest/gtest.h>

namespace {

using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace wh = asterion::warehouse;
namespace wire = ::warehouse;

class Warehouse : public ::testing::Test
{
protected:
    void TearDown() override
    {
        fail_next_directory_syncs_for_testing(0);
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    fs::path root = fs::temp_directory_path() / ("asterion-warehouse-" + unique_process_id());
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
            case wh::Kind::price:
                result.emplace_back(std::string("0.00000001"));
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
    wh::Store store(wh::source_directory(root, "research"), "research");
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
    wh::Store store(wh::source_directory(root, "research"), "research");
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

TEST_F(Warehouse, AChangedManifestCannotReuseAFixedVersion)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    const auto first = publish(store, "daily/SHFE/2026-10", 1);
    const auto second = publish(store, "daily/SHFE/2026-10", 2);
    fs::copy_file(
        store.root() / "versions" / (second.identity + ".json"),
        store.root() / "versions" / (first.identity + ".json"),
        fs::copy_options::overwrite_existing);
    EXPECT_THROW(store.version(first.identity), std::runtime_error);
    EXPECT_EQ(store.version(second.identity).partitions.at("daily/SHFE/2026-10").rows, 2);
}

TEST_F(Warehouse, EveryTablePreservesTypesPrecisionNullsAndText)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    auto writer = store.writer();
    for (const auto* table :
        {"daily",
            "calendar",
            "contracts",
            "minutes",
            "settlement",
            "price_ticks",
            "warehouse_receipts",
            "member_positions",
            "inventory"}) {
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
                case wh::Kind::price:
                    EXPECT_EQ(cell.type(), duckdb::LogicalType::DECIMAL(28, 8));
                    EXPECT_EQ(cell.ToString(), "0.00000001");
                    break;
            }
        }
    }
}

TEST_F(Warehouse, RefusesLossyInputAndDoesNotPublishIt)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
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

TEST_F(Warehouse, ResearchTakesTushareFirstAndOthersFillWhatItLacks)
{
    const auto store = [&](const char* provider) {
        return std::make_unique<wh::Store>(wh::source_directory(root, provider), provider);
    };
    const std::string mine = "minutes/SHFE/2026-10-29/rb/2027-01";
    const std::string tick = "price_ticks/SHFE/2026-10-30";
    auto tushare = store("tushare"), shfe = store("shfe"), gtja = store("gtja");
    publish(*tushare, "daily/SHFE/2026-10");
    publish(*tushare, "contracts/SHFE");
    publish(*tushare, mine, 2);
    publish(*tushare, "inventory/SHFE/2026-10-30");
    publish(*tushare, "inventory/SHFE/2026-10-29");
    publish(*shfe, "inventory/SHFE/2026-10-30", 2);
    publish(*shfe, "settlement/SHFE/2026-10-30");
    publish(*shfe, "daily/SHFE/2026-10-30");
    publish(*gtja, tick, 2);
    publish(*gtja, "price_ticks/SHFE/2026-10-29");
    wh::Store composed(wh::source_directory(root, "research"), "research");
    auto completed = wh::source_versions(root);
    const auto research = wh::compose(root, composed, completed);
    std::map<std::string, std::string> sources;
    for (const auto& [key, part] : research.partitions)
        sources[key] = part.source;
    EXPECT_EQ(
        sources,
        (std::map<std::string, std::string>{
            {"contracts/SHFE", "tushare"},
            {"daily/SHFE/2026-10", "tushare"},
            {"daily/SHFE/2026-10-30", "shfe"},
            {"inventory/SHFE/2026-10-29", "tushare"},
            {"inventory/SHFE/2026-10-30", "shfe"},
            {mine, "tushare"},
            {"price_ticks/SHFE/2026-10-29", "gtja"},
            {tick, "gtja"},
            {"settlement/SHFE/2026-10-30", "shfe"}
        }));
    // Files are linked, not copied.
    const wh::Store reader(wh::source_directory(root, "research"), "research");
    EXPECT_TRUE(fs::equivalent(reader.file(research, tick), gtja->file(gtja->current(), tick)));
    EXPECT_EQ(reader.read(research, mine).size(), 2U);
    EXPECT_EQ(reader.read(research, "inventory/SHFE/2026-10-30").size(), 2U);
    publish(*tushare, mine, 4);
    EXPECT_EQ(wh::compose(root, composed, completed).identity, research.identity);
    completed["tushare"] = tushare->current_identity();
    const auto later = wh::compose(root, composed, completed);
    EXPECT_EQ(reader.read(later, mine).size(), 4U);
    EXPECT_EQ(later.partitions.at(tick).source, "gtja");
    EXPECT_EQ(reader.read(research, mine).size(), 2U) << "earlier versions are unchanged";
}

TEST_F(Warehouse, CoverageSummarizesProductsAndListsEachContractsMinutes)
{
    const auto store = [&](const char* provider) {
        return std::make_unique<wh::Store>(wh::source_directory(root, provider), provider);
    };
    auto tushare = store("tushare");
    publish(*tushare, "minutes/SHFE/2026-09-30/rb/2027-01", 2);
    publish(*tushare, "minutes/SHFE/2026-10-29/rb/2027-01", 2);
    publish(*tushare, "minutes/SHFE/2026-10-30/rb/2027-01", 2);
    publish(*tushare, "minutes/SHFE/2026-10-30/cu/2026-12", 2);
    wh::Store composed(wh::source_directory(root, "research"), "research");
    wire::DatasetRef query;
    query.set_version(wh::compose(root, composed, wh::source_versions(root)).identity);
    const auto coverage = wh::Reader(root).coverage(query);
    ASSERT_EQ(coverage.products_size(), 2);
    EXPECT_EQ(coverage.products(0).product(), "SHFE/cu");
    const auto& rb = coverage.products(1);
    EXPECT_EQ(rb.product(), "SHFE/rb");
    EXPECT_EQ(rb.contracts(), 1);
    EXPECT_EQ(rb.minutes().held(), 1);
    wire::ProductRef product;
    product.set_version(query.version());
    product.set_product("SHFE/rb");
    const auto detail = wh::Reader(root).product_coverage(product);
    ASSERT_EQ(detail.contracts_size(), 1);
    EXPECT_EQ(detail.contracts(0).contract(), "SHFE/rb/2027-01");
    EXPECT_EQ(detail.contracts(0).minutes().days(), 3);
    EXPECT_EQ(detail.contracts(0).minutes().first_day(), "2026-09-30");
    EXPECT_EQ(detail.contracts(0).minutes().last_day(), "2026-10-30");
    product.clear_product();
    const auto all = wh::Reader(root).product_coverage(product);
    ASSERT_EQ(all.contracts_size(), 2);
    EXPECT_EQ(all.contracts(0).contract(), "SHFE/cu/2026-12");
    EXPECT_EQ(all.contracts(1).SerializeAsString(), detail.contracts(0).SerializeAsString());
}

TEST_F(Warehouse, MinuteCoverageIncludesEmptyDaysBeforeFirstStoredDay)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    auto writer = store.writer();
    std::vector<wh::Row> calendar;
    std::string empty;
    for (const auto* day :
        {"2026-10-12",
            "2026-10-13",
            "2026-10-14",
            "2026-10-15",
            "2026-10-16",
            "2026-10-19",
            "2026-10-20",
            "2026-10-21",
            "2026-10-22",
            "2026-10-23",
            "2026-10-26",
            "2026-10-27",
            "2026-10-28",
            "2026-10-29",
            "2026-10-30"}) {
        auto value = row("calendar");
        value[0] = std::string("CFFEX");
        value[1] = std::string(day);
        value[2] = true;
        calendar.push_back(value);
        const auto key = std::string("minutes/CFFEX/") + day + "/ic/2027-06";
        if (std::string_view(day) < "2026-10-19")
            empty += key + '\n';
        else
            writer.put(key, std::vector<wh::Row>{row("minutes")}, {day});
    }
    writer.put("calendar/CFFEX/2026", calendar);
    const auto version = writer.commit();
    fs::create_directories(root / "acquisition");
    replace_file_durably(wh::empty_file(root, "minutes"), empty);
    wire::ProductRef product;
    product.set_version(version.identity);
    product.set_product("CFFEX/ic");
    const auto detail = wh::Reader(root).product_coverage(product);
    ASSERT_EQ(detail.contracts_size(), 1);
    const auto& minutes = detail.contracts(0).minutes();
    EXPECT_EQ(minutes.days(), 10);
    EXPECT_EQ(minutes.empty_days(), 5);
    EXPECT_EQ(minutes.open_days(), 15);
    EXPECT_EQ(minutes.pending_days(), 0);
    EXPECT_EQ(minutes.gap_days(), 0);
}

TEST_F(Warehouse, VersionsListNewestFirstWithWhatEachAddedReplacedAndRemoved)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
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
    compare.set_version(second);
    compare.set_base(first);
    const auto change = reader.change(compare);
    EXPECT_EQ(change.added(), 1);
    EXPECT_EQ(change.replaced(), 1);
    EXPECT_EQ(change.removed(), 0);
}

TEST_F(Warehouse, ReferenceTablesPageByExchangeAndDays)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
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
        std::vector<wh::Row> days;
        for (int day = 1; day <= 25; ++day)
            for (int month = 7; month <= 10; ++month)
                days.push_back(with("calendar", 1, format_trading_date(2026y / month / day)));
        writer.put("calendar/SHFE/2026", days);
        writer.put(
            "calendar/SHFE/2025",
            std::vector<wh::Row>{with("calendar", 1, std::string("2025-12-31"))});
        writer.commit();
    }
    wh::Reader reader(root);
    wire::GetTableRows query;
    query.set_table(wire::TABLE_CONTRACTS);
    query.set_exchange("SHFE");
    const auto contracts = reader.rows(query);
    EXPECT_EQ(contracts.columns(1), "contract");
    ASSERT_EQ(contracts.total(), 2);
    EXPECT_EQ(contracts.rows(0).values(1), "SHFE/cu/2026-12");
    query.set_table(wire::TABLE_CALENDAR);
    const auto calendar = reader.rows(query);
    ASSERT_EQ(calendar.total(), 101);
    EXPECT_EQ(calendar.rows(0).values(1), "2026-10-25");
    query.set_offset(100);
    EXPECT_EQ(reader.rows(query).rows(0).values(1), "2025-12-31");
    query.set_offset(0);
    query.set_table(wire::TABLE_MINUTES);
    EXPECT_THROW(reader.rows(query), std::invalid_argument);
}

TEST_F(Warehouse, ReportPagesCrossDatesAndKeepTotalAtTheEnd)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    for (const auto* table :
        {"settlement", "warehouse_receipts", "member_positions", "inventory"}) {
        const auto day_column =
            std::string_view(table) == "member_positions" || std::string_view(table) == "settlement"
            ? 3
            : 2;
        {
            auto writer = store.writer();
            for (const auto& [day, count] :
                {std::pair{"2026-10-28", 80}, {"2026-10-29", 120}, {"2026-10-30", 80}}) {
                auto record = row(table);
                record[0] = std::string("SHFE");
                record[day_column] = std::string(day);
                writer.put(
                    std::string(table) + "/SHFE/" + day,
                    std::vector<wh::Row>(count, record));
            }
            writer.commit();
        }
        wh::Reader reader(root);
        wire::GetTableRows query;
        query.set_table(protocol::table_of(table));
        query.set_exchange("SHFE");
        query.set_offset(50);
        const auto crossing = reader.rows(query);
        ASSERT_EQ(crossing.total(), 280);
        ASSERT_EQ(crossing.rows_size(), 100);
        EXPECT_EQ(crossing.rows(29).values(day_column), "2026-10-30");
        EXPECT_EQ(crossing.rows(30).values(day_column), "2026-10-29");
        query.set_offset(180);
        const auto last = reader.rows(query);
        ASSERT_EQ(last.rows_size(), 100);
        EXPECT_EQ(last.rows(19).values(day_column), "2026-10-29");
        EXPECT_EQ(last.rows(20).values(day_column), "2026-10-28");
        query.set_offset(250);
        EXPECT_EQ(reader.rows(query).rows_size(), 30);
        query.set_offset(280);
        const auto end = reader.rows(query);
        EXPECT_EQ(end.total(), 280);
        EXPECT_EQ(end.rows_size(), 0);
    }
}

TEST_F(Warehouse, ReferenceTableFiltersByProductContractAndDay)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    {
        auto writer = store.writer();
        for (const auto& day : {"2026-10-28", "2026-10-29"}) {
            auto rb = row("warehouse_receipts");
            rb[0] = std::string("SHFE");
            rb[1] = std::string("rb");
            rb[2] = std::string(day);
            auto ag = row("warehouse_receipts");
            ag[0] = std::string("SHFE");
            ag[1] = std::string("ag");
            ag[2] = std::string(day);
            writer.put(std::string("warehouse_receipts/SHFE/") + day, std::vector<wh::Row>{rb, ag});

            auto s1 = row("settlement");
            s1[0] = std::string("SHFE");
            s1[1] = std::string("rb");
            s1[2] = std::string("SHFE/rb/2026-10");
            s1[3] = std::string(day);
            auto s2 = row("settlement");
            s2[0] = std::string("SHFE");
            s2[1] = std::string("rb");
            s2[2] = std::string("SHFE/rb/2027-01");
            s2[3] = std::string(day);
            writer.put(std::string("settlement/SHFE/") + day, std::vector<wh::Row>{s1, s2});
        }
        auto tick_old = row("price_ticks");
        tick_old[0] = std::string("SHFE");
        tick_old[1] = std::string("rb");
        tick_old[2] = std::string("SHFE/rb/2026-10");
        tick_old[3] = std::string("2025-01-01");
        tick_old[4] = std::string("2026-01-01");
        auto tick_now = row("price_ticks");
        tick_now[0] = std::string("SHFE");
        tick_now[1] = std::string("rb");
        tick_now[2] = std::string("SHFE/rb/2026-10");
        tick_now[3] = std::string("2026-01-01");
        tick_now[4] = std::monostate{};
        writer.put("price_ticks/SHFE", std::vector<wh::Row>{tick_old, tick_now});

        auto cal1 = row("calendar");
        cal1[0] = std::string("SHFE");
        cal1[1] = std::string("2026-10-28");
        auto cal2 = row("calendar");
        cal2[0] = std::string("SHFE");
        cal2[1] = std::string("2026-10-29");
        writer.put("calendar/SHFE/2026", std::vector<wh::Row>{cal1, cal2});
        writer.commit();
    }
    wh::Reader reader(root);
    {
        wire::GetTableRows query;
        query.set_table(wire::TABLE_WAREHOUSE_RECEIPTS);
        query.set_exchange("SHFE");
        query.set_product("AG");
        const auto by_product = reader.rows(query);
        ASSERT_EQ(by_product.total(), 2);
        EXPECT_EQ(by_product.rows(0).values(1), "ag");
        EXPECT_EQ(by_product.rows(0).values(2), "2026-10-29");
        EXPECT_EQ(by_product.rows(1).values(2), "2026-10-28");

        query.set_day("2026-10-28");
        const auto both = reader.rows(query);
        ASSERT_EQ(both.total(), 1);
        EXPECT_EQ(both.rows(0).values(1), "ag");
        EXPECT_EQ(both.rows(0).values(2), "2026-10-28");
    }
    {
        wire::GetTableRows query;
        query.set_table(wire::TABLE_SETTLEMENT);
        query.set_exchange("SHFE");
        query.set_product("rb2610");
        const auto by_compact = reader.rows(query);
        ASSERT_EQ(by_compact.total(), 2);
        EXPECT_EQ(by_compact.rows(0).values(2), "SHFE/rb/2026-10");
        EXPECT_EQ(by_compact.rows(1).values(2), "SHFE/rb/2026-10");

        query.set_day("2026-10-29");
        const auto exact = reader.rows(query);
        ASSERT_EQ(exact.total(), 1);
        EXPECT_EQ(exact.rows(0).values(3), "2026-10-29");
    }
    {
        wire::GetTableRows query;
        query.set_table(wire::TABLE_PRICE_TICKS);
        query.set_exchange("SHFE");
        query.set_day("2026-01-01");
        const auto effective = reader.rows(query);
        ASSERT_EQ(effective.total(), 1);
        EXPECT_EQ(effective.rows(0).values(3), "2026-01-01");
    }
    {
        wire::GetTableRows query;
        query.set_table(wire::TABLE_CALENDAR);
        query.set_exchange("SHFE");
        query.set_day("2026-10-28");
        const auto cal = reader.rows(query);
        ASSERT_EQ(cal.total(), 1);
        EXPECT_EQ(cal.rows(0).values(1), "2026-10-28");

        query.set_product("rb");
        EXPECT_THROW(reader.rows(query), std::invalid_argument);
        query.clear_product();
        query.set_day("2026-13-40");
        EXPECT_THROW(reader.rows(query), std::invalid_argument);
    }
}

TEST_F(Warehouse, ReadingAPartitionLoadsOnlyTheShardsItNeeds)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    publish(store, "minutes/SHFE/2026-10-28/rb/2027-01");
    const auto version = publish(store, "minutes/DCE/2026-10-28/i/2027-01");
    // Without the other month's shard, the full version can no longer be read.
    fs::remove(
        store.root() / "shards"
        / (store.head(version.identity).shards.at("minutes/DCE/2026-10") + ".json"));
    EXPECT_THROW(store.version(version.identity), Error);
    wh::Reader reader(root);
    wire::ReadDatasetFile file;
    file.mutable_dataset()->set_version(version.identity);
    file.set_key("minutes/SHFE/2026-10-28/rb/2027-01");
    EXPECT_FALSE(reader.file(file).data().empty());
    wire::GetDatasetVersion query;
    query.mutable_dataset()->CopyFrom(file.dataset());
    query.set_exchange("SHFE");
    EXPECT_EQ(reader.version(query).partitions_size(), 1);
}

TEST_F(Warehouse, PagedMetadataAndChunkedFilesStayOnSelectedVersion)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    publish(store, "daily/SHFE/202610");
    const auto old = publish(store, "calendar/SHFE/all");
    wh::Reader reader(root, 1);
    wire::GetDatasetVersion query;
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
    EXPECT_FALSE(
        reader.version({}).has_dataset() && !reader.version({}).dataset().version().empty());
    EXPECT_FALSE(fs::exists(root));
    wh::Store first(wh::source_directory(root, "research"), "research");
    const auto version = publish(first, "daily/SHFE/202610", 2);
    wire::DatasetRef summary;
    summary.set_version(version.identity);
    publish(first, "daily/SHFE/202610", 3);
    const auto coverage = reader.summary(summary);
    ASSERT_EQ(coverage.tables_size(), 9);
    EXPECT_EQ(coverage.tables(0).table(), wire::TABLE_DAILY);
    EXPECT_EQ(coverage.tables(0).rows(), 2);
    EXPECT_EQ(coverage.tables(0).first_day(), "2026-10-30");
    EXPECT_EQ(coverage.tables(0).last_day(), "2026-10-30");
    EXPECT_EQ(coverage.tables(1).rows(), 0);
    EXPECT_TRUE(coverage.tables(1).first_day().empty());
}

TEST_F(Warehouse, FreshnessCountsTheOpenDaysEachExchangeAndContractLacks)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    auto writer = store.writer();
    std::vector<wh::Row> calendar;
    for (const auto* day :
        {"2026-10-26", "2026-10-27", "2026-10-28", "2026-10-29", "2026-10-30", "2026-10-31"}) {
        auto value = row("calendar");
        value[0] = std::string("SHFE");
        value[1] = std::string(day);
        value[2] = std::string_view(day) != "2026-10-31";
        calendar.push_back(value);
    }
    writer.put("calendar/SHFE/2026", calendar);
    std::vector<wh::Row> contracts;
    for (const auto& [contract, delisted] :
        {std::pair{"SHFE/rb/2026-11", "2026-10-28"}, std::pair{"SHFE/rb/2027-01", "2027-01-15"}}) {
        auto value = row("contracts");
        value[1] = std::string(contract);
        value[7] = std::string(delisted);
        contracts.push_back(value);
    }
    writer.put("contracts/SHFE", contracts);
    for (const auto* key :
        {"daily/SHFE/2026-10-26",
            "daily/SHFE/2026-10-27",
            "daily/SHFE/2026-10-29",
            "daily/DCE/2026-10-30",
            "minutes/SHFE/2026-10-27/rb/2026-11",
            "minutes/SHFE/2026-10-28/rb/2026-11",
            "minutes/SHFE/2026-10-29/rb/2027-01"})
        writer.put(
            key,
            std::vector<wh::Row>{row(
                std::string_view(key).substr(0, std::string_view(key).find('/')))});
    const auto version = writer.commit();

    const auto open = wh::open_days(store, version, "2026-10-30");
    ASSERT_EQ(open.at("SHFE").size(), 5);
    // The exchange lacks the 28th and the 30th; one without a calendar is unknown.
    const auto daily = wh::holdings(store, version, "daily", open, {});
    EXPECT_EQ(daily.at("SHFE").last, "2026-10-29");
    EXPECT_EQ(daily.at("SHFE").through, "2026-10-30");
    EXPECT_EQ(daily.at("SHFE").missing, 2);
    EXPECT_TRUE(daily.at("DCE").through.empty());
    EXPECT_EQ(daily.at("DCE").missing, 0);
    // A delisted contract is complete at its delisting; a listed one lacks the 30th.
    const auto minutes = wh::holdings(store, version, "minutes", open, {});
    EXPECT_EQ(minutes.at("SHFE/rb/2026-11").through, "2026-10-28");
    EXPECT_EQ(minutes.at("SHFE/rb/2026-11").missing, 0);
    EXPECT_EQ(minutes.at("SHFE/rb/2027-01").missing, 1);
    // A day every source answered empty is not missing, and the last one ends the holding.
    const auto answered = wh::holdings(
        store,
        version,
        "daily",
        open,
        {"daily/SHFE/2026-10-28", "daily/SHFE/2026-10-30"});
    EXPECT_EQ(answered.at("SHFE").missing, 0);
    EXPECT_EQ(answered.at("SHFE").empty, 2);
    EXPECT_EQ(answered.at("SHFE").last, "2026-10-30");

    wire::DatasetRef query;
    const auto summary = wh::Reader(root).summary(query);
    EXPECT_EQ(summary.tables(0).table(), wire::TABLE_DAILY);
    EXPECT_EQ(summary.tables(0).expected_last_day(), "2026-10-30");
    EXPECT_EQ(summary.tables(0).missing(), 2);
    EXPECT_EQ(summary.tables(3).table(), wire::TABLE_MINUTES);
    EXPECT_EQ(summary.tables(3).missing(), 1);
    EXPECT_TRUE(summary.tables(1).expected_last_day().empty());
    // The record of days every source answered empty takes them out of the gaps.
    fs::create_directories(root / "acquisition");
    replace_file_durably(wh::empty_file(root, "daily"), "daily/SHFE/2026-10-28\n");
    const auto recorded = wh::Reader(root).summary(query);
    EXPECT_EQ(recorded.tables(0).missing(), 1);
    EXPECT_EQ(recorded.tables(0).source_empty(), 1);
}

TEST_F(Warehouse, CoverageSeparatesDaysAnUpdateBringsFromGaps)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    auto writer = store.writer();
    std::vector<wh::Row> calendar;
    for (const auto* day : {"2026-10-26", "2026-10-27", "2026-10-28", "2026-10-29", "2026-10-30"}) {
        auto value = row("calendar");
        value[0] = std::string("SHFE");
        value[1] = std::string(day);
        value[2] = true;
        calendar.push_back(value);
    }
    writer.put("calendar/SHFE/2026", calendar);
    auto contract = row("contracts");
    contract[0] = std::string("SHFE");
    contract[1] = std::string("SHFE/rb/2027-01");
    contract[2] = std::string("rb");
    contract[6] = std::string("2026-10-26");
    contract[7] = std::string("2027-01-15");
    writer.put("contracts/SHFE", std::vector<wh::Row>{contract});
    // Daily bars miss the 27th and end on the 28th; minutes end on the 27th.
    for (const auto* day : {"2026-10-26", "2026-10-28"}) {
        auto bar = row("daily");
        bar[0] = std::string("SHFE");
        bar[1] = std::string("rb");
        bar[2] = std::string("SHFE/rb/2027-01");
        bar[3] = std::string(day);
        writer.put(std::string("daily/SHFE/") + day, std::vector<wh::Row>{bar}, {day});
    }
    writer.put(
        "minutes/SHFE/2026-10-27/rb/2027-01",
        std::vector<wh::Row>{row("minutes")},
        {"2026-10-27"});
    const auto version = writer.commit();
    fs::create_directories(root / "acquisition");
    replace_file_durably(wh::empty_file(root, "daily"), "daily/SHFE/2026-10-30\n");

    wire::ProductRef product;
    product.set_version(version.identity);
    product.set_product("SHFE/rb");
    const auto detail = wh::Reader(root).product_coverage(product);
    ASSERT_EQ(detail.contracts_size(), 1);
    const auto& daily = detail.contracts(0).daily();
    EXPECT_EQ(daily.days(), 2);
    EXPECT_EQ(daily.open_days(), 5);
    // The 29th comes with an update; the 30th no source has; the 27th is a gap.
    EXPECT_EQ(daily.pending_days(), 1);
    EXPECT_EQ(daily.empty_days(), 1);
    EXPECT_EQ(daily.gap_days(), 1);
    ASSERT_EQ(daily.gaps_size(), 1);
    EXPECT_EQ(daily.gaps(0).first(), "2026-10-27");
    EXPECT_EQ(daily.gaps(0).last(), "2026-10-27");
    const auto& minutes = detail.contracts(0).minutes();
    // Counted from listing: the 26th is before the first stored day, not a gap.
    EXPECT_EQ(minutes.days(), 1);
    EXPECT_EQ(minutes.open_days(), 5);
    EXPECT_EQ(minutes.pending_days(), 3);
    EXPECT_EQ(minutes.gap_days(), 0);
}

TEST_F(Warehouse, CoverageCountsContractAbsentFromStoredDailyPartitionAsEmpty)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    auto writer = store.writer();
    std::vector<wh::Row> calendar;
    for (const auto* day : {"2026-10-28", "2026-10-29", "2026-10-30"}) {
        auto value = row("calendar");
        value[0] = std::string("SHFE");
        value[1] = std::string(day);
        value[2] = true;
        calendar.push_back(value);
    }
    writer.put("calendar/SHFE/2026", calendar);
    const auto contract = [](const char* id) {
        auto value = row("contracts");
        value[0] = std::string("SHFE");
        value[1] = std::string(id);
        value[2] = std::string("rb");
        value[6] = std::string("2026-10-28");
        value[7] = std::string("2027-02-15");
        return value;
    };
    writer.put(
        "contracts/SHFE",
        std::vector<wh::Row>{
            contract("SHFE/rb/2027-01"),
            contract("SHFE/rb/2027-02"),
            contract("SHFE/rb/2027-03")
        });
    const auto bar = [](const char* id, const char* day) {
        auto value = row("daily");
        value[0] = std::string("SHFE");
        value[1] = std::string("rb");
        value[2] = std::string(id);
        value[3] = std::string(day);
        return value;
    };
    writer.put(
        "daily/SHFE/2026-10-28",
        std::vector<wh::Row>{
            bar("SHFE/rb/2027-01", "2026-10-28"),
            bar("SHFE/rb/2027-02", "2026-10-28")
        },
        {"2026-10-28"});
    writer.put(
        "daily/SHFE/2026-10-29",
        std::vector<wh::Row>{bar("SHFE/rb/2027-01", "2026-10-29")},
        {"2026-10-29"});
    writer.put(
        "daily/SHFE/2026-10-30",
        std::vector<wh::Row>{
            bar("SHFE/rb/2027-01", "2026-10-30"),
            bar("SHFE/rb/2027-02", "2026-10-30")
        },
        {"2026-10-30"});
    const auto version = writer.commit();
    wire::DatasetRef query;
    query.set_version(version.identity);
    const auto coverage = wh::Reader(root).coverage(query);
    ASSERT_EQ(coverage.products_size(), 1);
    EXPECT_EQ(coverage.products(0).listed(), 3);
    EXPECT_EQ(coverage.products(0).daily().held(), 2);
    EXPECT_EQ(coverage.products(0).daily().with_gaps(), 0);
    EXPECT_EQ(coverage.products(0).daily().complete(), 2);
    EXPECT_EQ(coverage.products(0).daily().listed_complete(), 3);
    EXPECT_EQ(coverage.products(0).daily().gaps_size(), 0);
    wire::ProductRef product;
    product.set_version(version.identity);
    product.set_product("SHFE/rb");
    const auto detail = wh::Reader(root).product_coverage(product);
    ASSERT_EQ(detail.contracts_size(), 3);
    EXPECT_EQ(detail.contracts(0).daily().gap_days(), 0);
    EXPECT_EQ(detail.contracts(0).daily().empty_days(), 0);
    EXPECT_EQ(detail.contracts(1).daily().gap_days(), 0);
    EXPECT_EQ(detail.contracts(1).daily().empty_days(), 1);
    EXPECT_EQ(detail.contracts(1).daily().days(), 2);
    EXPECT_EQ(detail.contracts(2).daily().gap_days(), 0);
    EXPECT_EQ(detail.contracts(2).daily().empty_days(), 3);
    EXPECT_EQ(detail.contracts(2).daily().days(), 0);
}

TEST_F(Warehouse, ProductCoverageStaysOnOneVersionAndMissingCalendarStaysUnknown)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    const auto daily = [](const std::string& product,
                           const std::string& day,
                           const std::string& delivery = "2027-01") {
        auto value = row("daily");
        value[0] = std::string("SHFE");
        value[1] = product;
        value[2] = "SHFE/" + product + "/" + delivery;
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
                daily("cu", "2026-10-30", "2026-12"),
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
    EXPECT_THROW(reader.coverage(query), std::invalid_argument);
    query.set_version(first.identity);
    const auto coverage = reader.coverage(query);
    ASSERT_EQ(coverage.products_size(), 2);
    const auto& product = coverage.products(0);
    EXPECT_EQ(product.product(), "SHFE/cu");
    EXPECT_EQ(product.contracts(), 2);
    EXPECT_EQ(product.daily().held(), 2);
    EXPECT_EQ(product.daily().first_day(), "2026-09-30");
    EXPECT_EQ(product.daily().last_day(), "2026-10-30");
    EXPECT_EQ(product.daily().with_gaps(), 0);
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
    query.set_version(std::string(64, '0'));
    EXPECT_THROW(reader.coverage(query), Error);
}

::warehouse::Response call(testing_support::WarehouseChannel& channel, ::warehouse::Request request)
{
    request.set_id(1);
    channel.send(request.SerializeAsString(), 5s);
    ::warehouse::Response response;
    if (!response.ParseFromString(channel.receive(5s)))
        throw std::runtime_error("invalid Warehouse response");
    return response;
}

TEST_F(Warehouse, RemoteClientsKeepIndependentVersionsAndReconnect)
{
    testing_support::WarehouseHost host(root);
    wh::Store store(wh::source_directory(root / "data", "research"), "research");
    const auto old = publish(store, "daily/SHFE/202610");
    host.start();
    auto first = host.open();
    auto second = host.open();
    ::warehouse::Request request;
    request.mutable_get_dataset_version()->mutable_dataset()->set_version(old.identity);
    const auto baseline = call(first, request);
    ASSERT_TRUE(baseline.has_get_dataset_version()) << baseline.DebugString();
    EXPECT_EQ(baseline.get_dataset_version().dataset().version(), old.identity);
    EXPECT_EQ(call(second, request).get_dataset_version().partitions(0).rows(), 1);
    request.mutable_get_dataset_version()->mutable_dataset()->set_version(std::string(64, '0'));
    EXPECT_TRUE(call(first, request).has_error());
    request.mutable_get_status();
    EXPECT_TRUE(call(second, request).has_get_status());
    first = host.open();
    auto* file = request.mutable_read_dataset_file();
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

TEST_F(Warehouse, GarbageCollectionReclaimsUnreferencedPartitionsWhileProtectingPinnedVersions)
{
    wh::Store tushare(wh::source_directory(root, "tushare"), "tushare");
    wh::Store research(wh::source_directory(root, "research"), "research");

    const auto t1 = publish(tushare, "daily/SHFE/2026-01", 1);
    const auto r1 = wh::compose(root, research, wh::source_versions(root));
    const auto t2 = publish(tushare, "daily/SHFE/2026-01", 2);
    const auto r2 = wh::compose(root, research, wh::source_versions(root));
    const auto t3 = publish(tushare, "daily/SHFE/2026-01", 3);
    const auto r3 = wh::compose(root, research, wh::source_versions(root));

    const auto r2_file = research.file(r2, "daily/SHFE/2026-01");
    const auto t2_file = tushare.file(t2, "daily/SHFE/2026-01");
    const auto r1_file = research.file(r1, "daily/SHFE/2026-01");
    const auto t1_file = tushare.file(t1, "daily/SHFE/2026-01");
    ASSERT_TRUE(fs::is_regular_file(r2_file));
    ASSERT_TRUE(fs::is_regular_file(t2_file));
    ASSERT_TRUE(fs::is_regular_file(r1_file));
    ASSERT_TRUE(fs::is_regular_file(t1_file));

    // Write a keep-versions file protecting r1.
    const auto keep_file = root / "pinned.txt";
    {
        std::ofstream out(keep_file);
        out << "# pinned research run\n" << r1.identity << "\n\n";
    }
    const auto pinned = wh::read_keep_versions_file(keep_file);
    EXPECT_EQ(pinned, (std::set<std::string>{r1.identity}));

    // Dry run reports what would be removed without deleting anything.
    wh::GcOptions options;
    options.data_directory = root;
    options.keep_last = 1;
    options.keep_source_last = 1;
    options.keep_versions = pinned;
    options.apply = false;
    const auto dry = wh::collect_garbage(options);
    EXPECT_FALSE(dry.applied);
    EXPECT_TRUE(fs::is_regular_file(r2_file));
    EXPECT_TRUE(fs::is_regular_file(t2_file));

    // Refuses to run when an unknown pinned version is requested.
    auto bad = options;
    bad.keep_versions.insert(std::string(64, '0'));
    bad.apply = true;
    EXPECT_THROW(wh::collect_garbage(bad), std::invalid_argument);
    EXPECT_TRUE(fs::is_regular_file(r2_file));

    // Refuses to run while Warehouse holds the exclusive directory lock.
    {
        FileLock live_owner(root, "warehouse.lock");
        auto locked = options;
        locked.apply = true;
        EXPECT_THROW(wh::collect_garbage(locked), std::exception);
    }

    // Apply GC: deletes r2/t1/t2 unreferenced files while keeping r1 (pinned) and r3 (current).
    options.apply = true;
    const auto applied = wh::collect_garbage(options);
    EXPECT_TRUE(applied.applied);
    EXPECT_FALSE(fs::exists(r2_file));
    EXPECT_FALSE(fs::exists(t2_file));
    EXPECT_FALSE(fs::exists(t1_file));
    // Hard-linked copy in research stays intact for pinned version r1 even though t1 was removed!
    EXPECT_TRUE(fs::is_regular_file(r1_file));
    EXPECT_EQ(research.read(research.version(r1.identity), "daily/SHFE/2026-01").size(), 1u);
    EXPECT_EQ(research.read(research.version(r3.identity), "daily/SHFE/2026-01").size(), 3u);
    EXPECT_FALSE(research.has_version(r2.identity));

    // Reader::versions stops cleanly when encountering the pruned parent r2 of r3.
    wh::Reader reader(root);
    const auto history = reader.versions({});
    ASSERT_EQ(history.versions_size(), 1);
    EXPECT_EQ(history.versions(0).dataset().version(), r3.identity);
    EXPECT_TRUE(history.next_version().empty());
}

// Writes fixtures/record-format/sources/research/: a two-version composed
// Warehouse store covering all six tables (including daily price limits),
// shards, version chain and partitions. Regenerate only after changing the
// on-disk Warehouse format on purpose.
TEST_F(Warehouse, DISABLED_WriteRecordFormatFixture)
{
    wh::Store tushare(wh::source_directory(root, "tushare"), "tushare");
    wh::Store verified(wh::source_directory(root, "verified"), "verified");
    wh::Store research(wh::source_directory(root, "research"), "research");

    wh::CalendarDay day1{"SHFE", "2026-10-29", true, std::string("2026-10-28")};
    wh::CalendarDay day2{"SHFE", "2026-10-30", true, std::string("2026-10-29")};
    wh::Contract contract{
        .exchange = "SHFE",
        .contract = "SHFE/rb/2027-01",
        .product = "rb",
        .delivery_month = "2027-01",
        .source_instrument = "rb2701",
        .name = std::string("螺纹钢2701"),
        .list_date = "2026-01-16",
        .delist_date = "2027-01-15",
        .last_delivery_date = std::string("2027-01-22"),
        .multiplier = std::string("10"),
        .per_unit = std::string("1"),
        .trade_unit = std::string("吨"),
        .quote_unit = std::string("元/吨"),
        .tick_description = std::string("1元/吨"),
        .delivery_description = std::string("实物交割"),
        .trading_hours = std::string("09:00-15:00")
    };
    wh::DailyBar bar1{
        .exchange = "SHFE",
        .product = "rb",
        .contract = "SHFE/rb/2027-01",
        .day = "2026-10-29",
        .open = std::string("3400"),
        .high = std::string("3420"),
        .low = std::string("3390"),
        .close = std::string("3410"),
        .settlement = std::string("3405"),
        .previous_close = std::string("3395"),
        .previous_settlement = std::string("3398"),
        .volume = std::string("1000"),
        .amount = std::string("34050000"),
        .open_interest = std::string("50000"),
        .open_interest_change = std::string("120"),
        .delivery_settlement = std::monostate{},
        .lower_limit = std::string("3200"),
        .upper_limit = std::string("3600")
    };
    wh::DailyBar bar2 = bar1;
    bar2.day = "2026-10-30";
    bar2.open = std::string("3415");
    bar2.close = std::string("3425");
    bar2.lower_limit = std::string("3205");
    bar2.upper_limit = std::string("3605");
    wh::MinuteBar minute{
        .exchange = "SHFE",
        .product = "rb",
        .contract = "SHFE/rb/2027-01",
        .day = "2026-10-30",
        .start_ms = 1793322000000,
        .partial = false,
        .open = std::string("3415"),
        .high = std::string("3420"),
        .low = std::string("3414"),
        .close = std::string("3418"),
        .volume = std::string("42"),
        .average_price = std::string("3417"),
        .open_interest = std::string("50010")
    };
    wh::Settlement settle{
        .exchange = "SHFE",
        .product = "rb",
        .contract = "SHFE/rb/2027-01",
        .day = "2026-10-30",
        .long_margin_rate = std::string("0.10"),
        .short_margin_rate = std::string("0.10"),
        .fee_rate = std::string("0.0001"),
        .fee_per_lot = std::monostate{},
        .close_today_fee_rate = std::string("0.0001"),
        .close_today_fee_per_lot = std::monostate{}
    };
    wh::PriceTick tick{
        .exchange = "SHFE",
        .product = "rb",
        .contract = std::string("SHFE/rb/2027-01"),
        .effective_from = "2026-01-16",
        .effective_until = "2027-01-15",
        .price_tick = Decimal::parse("1"),
        .source_url = "fixture"
    };

    {
        auto writer = tushare.writer();
        writer.put(
            "calendar/SHFE/2026",
            wh::rows(std::vector{day1, day2}),
            {"2026-10-29", "2026-10-30"});
        writer.put("contracts/SHFE/all", wh::rows(std::vector{contract}));
        writer.put("daily/SHFE/2026-10", wh::rows(std::vector{bar1}), {"2026-10-29"});
        writer.put(
            "minutes/SHFE/2026-10-30/rb/2027-01",
            wh::rows(std::vector{minute}),
            {"2026-10-30"});
        writer.put("settlement/SHFE/2026-10", wh::rows(std::vector{settle}), {"2026-10-30"});
        writer.commit();
    }
    {
        auto writer = verified.writer();
        writer.put("price_ticks/SHFE/all", wh::rows(std::vector{tick}));
        writer.commit();
    }
    wh::compose(root, research, wh::source_versions(root));
    {
        auto writer = tushare.writer();
        writer.put(
            "daily/SHFE/2026-10",
            wh::rows(std::vector{bar1, bar2}),
            {"2026-10-29", "2026-10-30"});
        writer.commit();
    }
    wh::compose(root, research, wh::source_versions(root));

    const auto destination =
        fs::path(ASTERION_WAREHOUSE_FIXTURES) / "record-format/sources/research";
    fs::remove_all(fs::path(ASTERION_WAREHOUSE_FIXTURES) / "record-format");
    fs::create_directories(destination.parent_path());
    fs::copy(
        wh::source_directory(root, "research"),
        destination,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

// The committed Warehouse format fixture stays readable across versions,
// shards and all six table schemas without rewriting or mutating disk state.
TEST_F(Warehouse, ACommittedWarehouseRecordStillReads)
{
    const auto fixture = fs::path(ASTERION_WAREHOUSE_FIXTURES) / "record-format";
    wh::Store research(wh::source_directory(fixture, "research"), "research");
    const auto current = research.current();
    ASSERT_FALSE(current.identity.empty());
    ASSERT_FALSE(current.parent.empty());
    EXPECT_EQ(current.partitions.size(), 6U);

    const auto previous = research.version(current.parent);
    EXPECT_TRUE(previous.parent.empty());
    EXPECT_EQ(previous.partitions.size(), 6U);
    const auto diff =
        research.compare(research.head(current.identity), research.head(previous.identity));
    EXPECT_EQ(diff.added, 0U);
    EXPECT_EQ(diff.replaced, 1U);
    EXPECT_EQ(diff.removed, 0U);

    const auto daily_v1 = research.read(previous, "daily/SHFE/2026-10");
    ASSERT_EQ(daily_v1.size(), 1U);
    EXPECT_EQ(daily_v1[0].size(), wh::columns("daily").size());
    EXPECT_EQ(Decimal::parse(std::get<std::string>(daily_v1[0][16])), Decimal::parse("3200"));
    EXPECT_EQ(Decimal::parse(std::get<std::string>(daily_v1[0][17])), Decimal::parse("3600"));

    const auto daily_v2 = research.read(current, "daily/SHFE/2026-10");
    ASSERT_EQ(daily_v2.size(), 2U);
    EXPECT_EQ(Decimal::parse(std::get<std::string>(daily_v2[1][16])), Decimal::parse("3205"));
    EXPECT_EQ(Decimal::parse(std::get<std::string>(daily_v2[1][17])), Decimal::parse("3605"));

    EXPECT_EQ(research.read(current, "calendar/SHFE/2026").size(), 2U);
    EXPECT_EQ(research.read(current, "contracts/SHFE/all").size(), 1U);
    EXPECT_EQ(research.read(current, "minutes/SHFE/2026-10-30/rb/2027-01").size(), 1U);
    EXPECT_EQ(research.read(current, "settlement/SHFE/2026-10").size(), 1U);
    EXPECT_EQ(research.read(current, "price_ticks/SHFE/all").size(), 1U);

    wh::Reader reader(fixture);
    const auto history = reader.versions({});
    ASSERT_EQ(history.versions_size(), 2);
    EXPECT_EQ(history.versions(0).dataset().version(), current.identity);
    EXPECT_EQ(history.versions(1).dataset().version(), previous.identity);

    wire::DatasetRef query;
    query.set_version(current.identity);
    wire::GetDatasetVersion version_req;
    *version_req.mutable_dataset() = query;
    const auto manifest = reader.version(version_req);
    ASSERT_EQ(manifest.partitions_size(), 6);
    for (const auto& partition : manifest.partitions()) {
        EXPECT_FALSE(partition.sha256().empty());
        EXPECT_GT(partition.rows(), 0U);
    }
    const auto coverage = reader.coverage(query);
    ASSERT_EQ(coverage.products_size(), 1);
    EXPECT_EQ(coverage.products(0).product(), "SHFE/rb");
}

TEST_F(Warehouse, CachedSummaryAndCoverageRefreshWhenVersionOrEmptyKeysChange)
{
    wh::Store store(wh::source_directory(root, "research"), "research");
    {
        auto writer = store.writer();
        writer.put(
            "calendar/SHFE/2026",
            std::vector<wh::Row>{
                {"SHFE", "2026-01-05", true, "2026-01-02"},
                {"SHFE", "2026-01-06", true, "2026-01-05"},
                {"SHFE", "2026-01-07", true, "2026-01-06"}
            });
        writer.put(
            "contracts/SHFE",
            std::vector<wh::Row>{
                {"SHFE",
                    "SHFE/rb/2026-05",
                    "rb",
                    "2026-05",
                    "rb2605",
                    "螺纹钢",
                    "2026-01-05",
                    "2026-01-07",
                    "2026-01-07",
                    "10",
                    "1",
                    "吨",
                    "元",
                    "1",
                    "",
                    ""}
            });
        auto bar = row("daily");
        bar[0] = std::string("SHFE");
        bar[1] = std::string("rb");
        bar[2] = std::string("SHFE/rb/2026-05");
        bar[3] = std::string("2026-01-05");
        writer.put("daily/SHFE/2026-01-05", std::vector<wh::Row>{bar}, {"2026-01-05"});
        bar[3] = std::string("2026-01-07");
        writer.put("daily/SHFE/2026-01-07", std::vector<wh::Row>{bar}, {"2026-01-07"});
        writer.commit();
    }
    const auto v1 = store.current_identity();
    wh::Reader reader(root);

    wire::DatasetRef current_ref;
    const auto summary1 = reader.summary(current_ref);
    ASSERT_EQ(summary1.dataset().version(), v1);
    EXPECT_EQ(summary1.tables(0).missing(), 1U);
    EXPECT_EQ(summary1.tables(0).source_empty(), 0U);

    wire::DatasetRef v1_ref;
    v1_ref.set_version(v1);
    const auto cov1 = reader.coverage(v1_ref);
    ASSERT_EQ(cov1.products_size(), 1);
    EXPECT_EQ(cov1.products(0).daily().with_gaps(), 1U);

    // Product detail uses the cached full coverage without changing its output.
    wire::ProductRef prod_ref;
    prod_ref.set_version(v1);
    prod_ref.set_product("SHFE/rb");
    const auto detail1 = reader.product_coverage(prod_ref);
    ASSERT_EQ(detail1.contracts_size(), 1);
    EXPECT_EQ(detail1.contracts(0).daily().gap_days(), 1U);

    // Recording a source-empty day invalidates cached summary and coverage.
    fs::create_directories(root / "acquisition");
    std::ofstream(root / "acquisition/empty-daily.txt") << "daily/SHFE/2026-01-06\n";
    const auto summary2 = reader.summary(current_ref);
    EXPECT_EQ(summary2.tables(0).missing(), 0U);
    EXPECT_EQ(summary2.tables(0).source_empty(), 1U);
    const auto cov2 = reader.coverage(v1_ref);
    EXPECT_EQ(cov2.products(0).daily().with_gaps(), 0U);
    const auto detail2 = reader.product_coverage(prod_ref);
    EXPECT_EQ(detail2.contracts(0).daily().gap_days(), 0U);
    EXPECT_EQ(detail2.contracts(0).daily().empty_days(), 1U);
}

TEST_F(Warehouse, IncrementalCommitAndCachedComposeMatchFreshCompose)
{
    wh::Store tushare(wh::source_directory(root, "tushare"), "tushare");
    wh::Store shfe(wh::source_directory(root, "shfe"), "shfe");
    wh::Store cached_research(wh::source_directory(root, "research"), "research");
    {
        auto w = tushare.writer();
        auto bar = row("daily");
        bar[0] = std::string("SHFE");
        bar[1] = std::string("rb");
        bar[2] = std::string("SHFE/rb/2026-05");
        bar[3] = std::string("2026-01-05");
        w.put("daily/SHFE/2026-01-05", std::vector<wh::Row>{bar}, {"2026-01-05"});
        bar[3] = std::string("2026-02-02");
        w.put("daily/SHFE/2026-02-02", std::vector<wh::Row>{bar}, {"2026-02-02"});
        w.commit();
    }
    wh::SourceCache cache;
    const auto v1 = wh::compose(root, cached_research, wh::source_versions(root), cache);
    const auto v1_head = cached_research.head(v1.identity);
    ASSERT_EQ(v1.partitions.size(), 2U);
    ASSERT_EQ(v1_head.shards.size(), 2U);

    // Updating one shard in tushare and adding a fallback day in shfe preserves the untouched shard digest.
    {
        auto w = tushare.writer();
        auto bar = row("daily");
        bar[0] = std::string("SHFE");
        bar[1] = std::string("rb");
        bar[2] = std::string("SHFE/rb/2026-05");
        bar[3] = std::string("2026-02-03");
        w.put("daily/SHFE/2026-02-03", std::vector<wh::Row>{bar}, {"2026-02-03"});
        w.commit();
    }
    {
        auto w = shfe.writer();
        auto bar = row("daily");
        bar[0] = std::string("SHFE");
        bar[1] = std::string("rb");
        bar[2] = std::string("SHFE/rb/2026-05");
        bar[3] = std::string("2026-01-06");
        w.put("daily/SHFE/2026-01-06", std::vector<wh::Row>{bar}, {"2026-01-06"});
        w.commit();
    }
    const auto versions = wh::source_versions(root);
    const auto v2 = wh::compose(root, cached_research, versions, cache);
    const auto v2_head = cached_research.head(v2.identity);
    EXPECT_EQ(v2.partitions.size(), 4U);
    EXPECT_NE(v2_head.shards.at("daily/SHFE/2026-01"), v1_head.shards.at("daily/SHFE/2026-01"));
    EXPECT_NE(v2_head.shards.at("daily/SHFE/2026-02"), v1_head.shards.at("daily/SHFE/2026-02"));

    // Fresh compose on the exact same inputs yields the exact same partitions and shard digests.
    wh::Store fresh_research(root / "fresh", "research");
    const auto fresh = wh::compose(root, fresh_research, versions);
    EXPECT_EQ(fresh.partitions, v2.partitions);
    EXPECT_EQ(fresh_research.head(fresh.identity).shards, v2_head.shards);
}

} // namespace
