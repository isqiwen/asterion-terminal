#include "data/history_fixture.hpp"
#include <asterion/kernel/environment.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
int main(int argc, char** argv) {
  CLI::App app{"Seed isolated test history"};
  std::string directory, id, product = "rb", day = "2026-09-25";
  std::string data_instance = "historical-data";
  std::vector<int> prices;
  std::vector<std::string> minute_days, daily_days;
  std::string month = "2026-10";
  int open_interest = 100, settlement = 110;
  app.add_option("--directory", directory)->required();
  app.add_option("--data-instance", data_instance);
  app.add_option("--id", id)->required();
  app.add_option("--price", prices)->required();
  app.add_option("--product", product, "SHFE product of the 2026-10 contract");
  app.add_option("--day", day, "Trading day for the isolated fixture");
  app.add_option("--minute-days", minute_days);
  app.add_option("--daily-days", daily_days);
  app.add_option("--month", month, "Delivery month of the contract");
  app.add_option("--open-interest", open_interest, "Open interest of every daily row");
  app.add_option("--settlement", settlement, "Settlement price of every daily row");
  CLI11_PARSE(app, argc, argv);
  try {
    asterion::validate_id(data_instance);
    const auto root = asterion::environment_path("ASTERION_NODE_DIRECTORY");
    if (asterion::environment_variable("ASTERION_TEST_NODE_ISOLATED") != "1" || !root ||
        std::filesystem::canonical(directory) !=
            std::filesystem::canonical(*root / "services" / data_instance / "ledger"))
      throw std::invalid_argument("fixture requires isolated data warehouse");
    std::cout << asterion::test::seed_history(
                     directory, prices, id,
                     minute_days.empty() ? std::vector<std::string>{day} : minute_days, daily_days,
                     product, 1, settlement, "test.confirmed.v1", month, open_interest,
                     data_instance)
                     .dump()
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
