#include "history_fixture.hpp"
#include <asterion/kernel/environment.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
int main(int argc, char** argv) {
  CLI::App app{"Seed isolated test history"};
  std::string directory, id, product = "rb", day = "2026-09-25";
  std::vector<int> prices;
  app.add_option("--directory", directory)->required();
  app.add_option("--id", id)->required();
  app.add_option("--price", prices)->required();
  app.add_option("--product", product, "SHFE product of the 2026-10 contract");
  app.add_option("--day", day, "Trading day for the isolated fixture");
  CLI11_PARSE(app, argc, argv);
  try {
    const auto root = asterion::environment_path("ASTERION_NODE_DIRECTORY");
    if (asterion::environment_variable("ASTERION_TEST_NODE_ISOLATED") != "1" || !root ||
        std::filesystem::canonical(directory) !=
            std::filesystem::canonical(*root / "services" / "research" / "ledger"))
      throw std::invalid_argument("fixture requires isolated research ledger");
    std::cout << asterion::test::seed_history(directory, prices, id, {day}, {}, product).dump()
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
