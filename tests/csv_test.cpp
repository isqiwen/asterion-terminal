#include <gtest/gtest.h>
#include "csv_market_data.hpp"

#include <chrono>
#include <fstream>
#include <iostream>

using namespace asterion;
namespace {
struct TempInput {
    std::filesystem::path folder = std::filesystem::temp_directory_path() /
        ("asterion-csv-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempInput() { EXPECT_TRUE((std::filesystem::create_directory(folder))) << "unique test directory"; }
    ~TempInput() { std::error_code error; std::filesystem::remove_all(folder, error); }
    auto write(const std::string& content) const {
        const auto path = folder / "trades.csv";
        std::ofstream out(path, std::ios::binary);
        out << content;
        out.close();
        EXPECT_TRUE((static_cast<bool>(out))) << "write test input";
        return path;
    }
};
Instrument spec() {
    return {{"TEST", "BTC-USD"}, AssetClass::crypto, "USD", Decimal::parse("0.01"), Decimal::parse("0.001"), Decimal::parse("1")};
}
}
TEST(Csv, Contracts) {

        TempInput file;
        {
            const auto path = file.write("timestamp_ns,price,quantity\r\n100,12.34,0.001\r\n100,12.35,0.002\r\n101,12.36,0.001\r\n");
            PluginManager manager;
            auto plugin = std::make_unique<CsvMarketData>(path, spec());
            auto* source = plugin.get();
            EXPECT_THROW(([&] { source->next(); })(), std::logic_error);
            manager.add(std::move(plugin));
            manager.start();
            EXPECT_TRUE((source->next()->price == Decimal::parse("12.34"))) << "first trade";
            EXPECT_TRUE((source->next()->timestamp_ns == 100)) << "same timestamp permitted";
            EXPECT_TRUE((source->next()->timestamp_ns == 101)) << "next timestamp";
            EXPECT_TRUE((!source->next() && !source->next())) << "confirmed EOF";
            manager.stop();
            manager.start();
            EXPECT_TRUE((source->next()->timestamp_ns == 100)) << "restart explicitly rewinds source";
        }
        for (const char* row : {"99,12.34,0.001", "101,12.345,0.001", "101,12.34,0.0001", "101,12.34,0", "101,12.34,0.001,extra", "bad,12.34,0.001", "", "101,nan,0.001"}) {
            const auto path = file.write(std::string("timestamp_ns,price,quantity\n100,12.34,0.001\n") + row + "\n");
            CsvMarketData source(path, spec());
            source.start();
            EXPECT_TRUE((source.next().has_value())) << "valid prefix";
            EXPECT_THROW(([&] { source.next(); })(), std::runtime_error);
            EXPECT_THROW(([&] { source.next(); })(), std::logic_error);
        }
        {
            const auto path = file.write("timestamp_ns,price,quantity\n");
            CsvMarketData empty(path, spec());
            empty.start();
            EXPECT_TRUE((!empty.next())) << "empty valid file is EOF";
        }
        for (const char* contents : {"", "timestamp,price,quantity\n", "timestamp_ns,price,quantity,unknown\n"}) {
            const auto path = file.write(contents);
            CsvMarketData invalid(path, spec());
            EXPECT_THROW(([&] { invalid.start(); })(), std::invalid_argument);
            EXPECT_THROW(([&] { invalid.next(); })(), std::logic_error);
        }
        CsvMarketData missing(file.folder / "missing.csv", spec());
        EXPECT_THROW(([&] { missing.start(); })(), std::runtime_error);
}
