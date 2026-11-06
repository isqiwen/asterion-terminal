#include "acquisition/catalog_source.hpp"
#include "acquisition/manager.hpp"
#include "acquisition/snapshot_intake.hpp"
#include "warehouse/research.hpp"
#include <asterion/quotes.pb.h>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <thread>

namespace {

using namespace asterion;
using namespace std::chrono_literals;
namespace ac = asterion::history::acquisition;
namespace wh = asterion::history::warehouse;
namespace fs = std::filesystem;

class CatalogSource : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("catalog-source-" + unique_process_id());

    void TearDown() override
    {
        fs::remove_all(root);
    }

    void snapshot(const std::string& day, const std::string& contract, const std::string& tick)
    {
        ::quotes::Catalog catalog;
        auto* item = catalog.add_contracts();
        item->set_contract(contract);
        item->mutable_price_tick()->set_text(tick);
        fs::create_directories(root / "catalog");
        std::ofstream(root / "catalog" / (day + ".pb"), std::ios::binary)
            << catalog.SerializeAsString();
    }

    ac::Factory factory()
    {
        return [this](std::stop_token stop) {
            return std::make_unique<ac::CatalogSource>(root / "catalog", stop);
        };
    }
};

TEST_F(CatalogSource, ADaysSnapshotBecomesThatDaysPriceTicksAndOutranksReports)
{
    snapshot("20261028", "SHFE/rb/2027-01", "1");
    ac::Manager manager(root, {{"ctp", factory()}});
    ac::SnapshotIntake intake(root / "catalog", root, manager);
    wh::Store store(wh::source_directory(root, "ctp"), "ctp");
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (store.current().partitions.empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    intake.stop();
    wh::Store report(wh::source_directory(root, "gtja"), "gtja");
    {
        auto writer = report.writer();
        writer.put("price_ticks/SHFE/2026-10-28", std::vector<wh::Row>{}, {"2026-10-28"});
        writer.commit();
    }
    wh::Store research(wh::source_directory(root, "research"), "research");
    const auto version = wh::compose_research(root, research);
    EXPECT_EQ(version.partitions.at("price_ticks/SHFE/2026-10-28").source, "ctp");
    const auto rows = research.read(version, "price_ticks/SHFE/2026-10-28");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<std::string>(rows[0][2]), "SHFE/rb/2027-01");
    EXPECT_EQ(std::get<std::string>(rows[0][3]), "2026-10-28");
    EXPECT_EQ(std::get<std::string>(rows[0][4]), "2026-10-29");
    EXPECT_EQ(std::get<std::string>(rows[0][5]), "1.00000000");
}

TEST_F(CatalogSource, ADayStillTradingWaitsAndADamagedOneDoesNotHoldBackOthers)
{
    snapshot("20261028", "SHFE/rb/2027-01", "1");
    snapshot("20261029", "SHFE/rb/2027-01", "1");
    std::ofstream(root / "catalog/20261027.pb") << "not a catalog";
    ac::Manager manager(root, {{"ctp", factory()}});
    wh::Store store(wh::source_directory(root, "ctp"), "ctp");
    // Its thread may already have polled once with the real clock: use a later root day.
    ac::SnapshotIntake intake(root / "catalog", root, manager);
    intake.stop();
    const auto wall = [](int day, int hour) {
        return std::chrono::system_clock::time_point(
            std::chrono::sys_days{std::chrono::year{2026} / 10 / day}
            + std::chrono::hours(hour - 8));
    };
    const auto settle = [&](auto when) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (int polls = 0; polls < 40 && std::chrono::steady_clock::now() < deadline; ++polls) {
            intake.poll(std::chrono::steady_clock::now(), when);
            std::this_thread::sleep_for(10ms);
        }
    };
    settle(wall(29, 9));
    EXPECT_TRUE(store.current().partitions.contains("price_ticks/SHFE/2026-10-28"));
    EXPECT_FALSE(store.current().partitions.contains("price_ticks/SHFE/2026-10-27"));
}

} // namespace
