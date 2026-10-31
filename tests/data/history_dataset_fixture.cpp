#include "data/history_fixture.hpp"
#include "data/shfe_margin_fixture.hpp"
#include <asterion/system/environment.hpp>
#include <CLI/CLI.hpp>
#include <iostream>

int main(int argc, char** argv)
{
    CLI::App app{"Seed isolated test history"};
    std::string directory, id, product = "rb", day = "2026-09-25";
    std::string data_instance = "historical-data";
    std::vector<int> prices;
    std::vector<std::string> minute_days, daily_days;
    std::string month = "2026-10";
    int open_interest = 100, settlement = 110, daily_volume = 10000;
    bool tables_only = false, daily_prices = false, shfe_margins = false;
    asterion::test::DailyTableSources tables;
    app.add_option("--directory", directory)->required();
    app.add_option("--data-instance", data_instance);
    app.add_option("--id", id)->required();
    app.add_option("--price", prices)->required();
    app.add_option("--product", product, "SHFE product of the 2026-10 contract");
    app.add_option("--day", day, "Trading day for the isolated fixture");
    app.add_option("--minute-days", minute_days);
    app.add_option("--daily-days", daily_days);
    app.add_option("--daily-volume", daily_volume)->check(CLI::NonNegativeNumber);
    app.add_flag(
        "--daily-prices",
        daily_prices,
        "Use the test price sequence for daily OHLC and settlement");
    app.add_flag("--shfe-margins", shfe_margins, "Seed isolated SHFE margin report fixtures");
    app.add_flag("--tables-only", tables_only, "Seed daily tables without a minute archive");
    app.add_option("--month", month, "Delivery month of the contract");
    app.add_option("--open-interest", open_interest, "Open interest of every daily row");
    app.add_option("--settlement", settlement, "Settlement price of every daily row");
    app.add_option("--daily-source", tables.daily, "Source of the market-wide daily table");
    app.add_option("--calendar-source", tables.calendar, "Source of the trading calendar");
    app.add_option("--contracts-source", tables.contracts, "Source of the contract list");
    CLI11_PARSE(app, argc, argv);
    try {
        asterion::validate_id(data_instance);
        const auto root = asterion::environment_path("ASTERION_ROOT");
        if (asterion::environment_variable("ASTERION_TEST_NODE_ISOLATED") != "1" || !root
            || std::filesystem::canonical(directory)
                != std::filesystem::canonical(*root / "data" / "data" / data_instance))
            throw std::invalid_argument("fixture requires isolated data warehouse");
        if (tables_only) {
            {
                asterion::data::Store store(directory, data_instance);
            }
            if (shfe_margins)
                (void)asterion::test::seed_shfe_margins(directory);
            asterion::Json table;
            if (daily_prices) {
                if (daily_days.size() != prices.size() || prices.empty())
                    throw std::invalid_argument("daily fixture dates and prices must match");
                std::vector<asterion::test::DailyRow> rows;
                for (std::size_t i = 0; i < prices.size(); ++i) {
                    const auto open = i ? prices[i - 1] : prices[i];
                    rows.push_back(
                        {daily_days[i],
                            prices[i],
                            open_interest,
                            asterion::test::DailyTrade{
                                open,
                                std::max(open, prices[i]) + 3,
                                std::min(open, prices[i]) - 3,
                                prices[i],
                                daily_volume
                            }});
                }
                table = asterion::test::seed_daily_table(
                    directory,
                    "SHFE/" + product + "/" + month,
                    rows,
                    tables);
            } else {
                table = asterion::test::seed_daily_table(
                    directory,
                    "SHFE/" + product + "/" + month,
                    daily_days.empty() ? std::vector<std::string>{day} : daily_days,
                    settlement,
                    open_interest,
                    tables);
            }
            std::cout
                << asterion::
                       Json{{"source_dataset_ids", asterion::Json::array()}, {"daily", table}, {"begin_day", ""}, {"end_day", ""}}
                           .dump()
                << '\n';
            return 0;
        }
        std::cout << asterion::test::seed_history(
            directory,
            prices,
            id,
            minute_days.empty() ? std::vector<std::string>{day} : minute_days,
            daily_days,
            product,
            1,
            settlement,
            "test.confirmed.v1",
            month,
            open_interest,
            data_instance,
            tables)
                         .dump()
                  << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
