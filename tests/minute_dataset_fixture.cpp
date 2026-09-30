#include <chrono>
#include <cstdio>
// Seed only an isolated, stopped test Task Service through its real persistence API.
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include "task_store.hpp"
#include "tushare.hpp"
#include <asterion/kernel/environment.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
using namespace asterion;
namespace {
// Answers Tushare trading-calendar requests with every day open, so fixtures
// keep their own dates; every other request goes to the wrapped transport.
template <class Transport> tushare::Post calendar(Transport transport) {
  return [transport](const std::string& body, std::stop_token stop) -> std::string {
    const auto request = Json::parse(body);
    if (request.at("api_name") != "trade_cal")
      return transport(body, stop);
    const auto& params = request.at("params");
    const auto year = std::stoi(params.at("start_date").get<std::string>().substr(0, 4));
    Json items = Json::array();
    for (std::chrono::sys_days day = std::chrono::year(year) / 1 / 1;
         day <= std::chrono::sys_days(std::chrono::year(year) / 12 / 31);
         day += std::chrono::days(1)) {
      const std::chrono::year_month_day date(day);
      char text[9];
      std::snprintf(text, sizeof text, "%04d%02u%02u", static_cast<int>(date.year()),
                    static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
      items.push_back({params.at("exchange"), text, 1});
    }
    return Json{{"code", 0},
                {"data", {{"fields", {"exchange", "cal_date", "is_open"}}, {"items", items}}}}
        .dump();
  };
}
} // namespace
int main(int argc, char** argv) {
  CLI::App app{"Create an isolated native desktop minute-data fixture"};
  std::string directory;
  app.add_option("--directory", directory)->required();
  CLI11_PARSE(app, argc, argv);
  try {
    const auto root = environment_path("ASTERION_NODE_DIRECTORY");
    if (environment_variable("ASTERION_TEST_NODE_ISOLATED") != "1" || !root ||
        std::filesystem::canonical(directory) !=
            std::filesystem::canonical(*root / "services" / "research" / "ledger"))
      throw std::invalid_argument("minute fixture requires an isolated research ledger");
    tasks::Store store(directory); // Exclusive ownership refuses a running service.
    if (store.list().tasks_size())
      throw std::invalid_argument("minute fixture requires an empty task store");
    const auto begin = tushare::parse_time("2023-08-25 09:00:00");
    const HistoricalBarRange range{
        {"SHFE", "cu", "2023-10"}, 1,           begin, begin + 119 * 60000000000LL,
        "tushare.ft_mins",         "CU2310.SHF"};
    data::v1::MinuteDownload input;
    input.set_version(2);
    input.set_source("tushare.ft_mins");
    input.set_source_instrument("CU2310.SHF");
    input.set_contract_id("SHFE/cu/2023-10");
    input.set_interval_minutes(1);
    input.set_begin_ns(range.begin_ns);
    input.set_end_ns(range.end_ns);
    input.set_requests_per_minute(60);
    const std::string id = "native-minute-fixture";
    store.submit(id, input, "explicit-test-fixture");
    research::v1::TaskAttempt attempt;
    attempt.set_token(store.claim(id));
    *attempt.mutable_task() = store.get(id);
    store.download_attempt(attempt);
    tushare::Minutes provider(
        "explicit-test-fixture", calendar([begin](const auto&, auto) {
          std::string response =
              R"({"code":0,"data":{"fields":["ts_code","trade_time","open","high","low","close","vol","amount","oi"],"items":[)";
          for (int i = 0; i < 120; ++i) {
            if (i)
              response += ',';
            response += "[\"CU2310.SHF\",\"" + tushare::format_time(begin + i * 60000000000LL) +
                        "\",100.00000001," + std::to_string(102 + i % 7) + ",99," +
                        std::to_string(101 + i % 7) + ".5,20,12345678.12345678,1000]";
          }
          return response + "]}}";
        }));
    provider.start();
    history_files::download_minutes(provider, range, attempt.output_directory(), 60);
    research::v1::TaskFinish finish;
    finish.set_id(id);
    finish.set_token(attempt.token());
    *finish.mutable_minutes() = history_files::minute_result(attempt.output_directory());
    auto completion = store.prepare_finish(finish);
    completion.verify();
    store.finish(std::move(completion));

    const auto daily = history_files::daily_request({{"version", 2},
                                                     {"contract_id", "SHFE/cu/2023-10"},
                                                     {"source", "tushare.fut_daily"},
                                                     {"source_instrument", "CU2310.SHF"},
                                                     {"begin_day", "2023-01-01"},
                                                     {"end_day", "2023-04-30"},
                                                     {"requests_per_minute", 500}});
    const std::string daily_id = "native-daily-fixture";
    store.submit(daily_id, daily, "explicit-test-fixture");
    // Leave the task QUEUED. Only the real Agent-dispatched worker may claim
    // and finish it after Electron starts the isolated research service.
    const auto daily_directory = std::filesystem::path(directory) / "history" / "SHFE" / "cu" /
                                 "2023-10" / "tushare.fut_daily" / "daily" / daily_id;
    std::filesystem::create_directories(daily_directory);
    tushare::Daily daily_provider("explicit-test-fixture", [](const auto&, auto) {
      std::string response =
          R"({"code":0,"data":{"fields":["ts_code","trade_date","pre_close","pre_settle","open","high","low","close","settle","vol","amount","oi"],"items":[)";
      for (int i = 0; i < 120; ++i) {
        if (i)
          response += ',';
        using namespace std::chrono;
        auto date =
            format_trading_date(year_month_day(sys_days(year(2023) / January / 1) + days(i)));
        std::erase(date, '-');
        response += "[\"CU2310.SHF\",\"" + date + "\",null,99.5,100.00000001," +
                    std::to_string(102 + i % 7) + ",99," + std::to_string(101 + i % 7) +
                    ".5,null,20,1234.567812345678,1000]";
      }
      return response + "]}}";
    });
    daily_provider.start();
    history_files::download_daily(daily_provider, history_files::daily_range(daily),
                                  daily_directory, 500);
    if (store.get(daily_id).state() != research::v1::QUEUED || store.get(daily_id).attempt() != 0)
      throw std::runtime_error("daily fixture must remain queued for Agent dispatch");
    std::cout
        << "Created completed minute fixture and queued daily fixture with durable source pages\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
