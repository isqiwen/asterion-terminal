#include "timing.hpp"
#include <gtest/gtest.h>
#include <asterion/terminal.h>
#include <asterion/domain/futures.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <atomic>
#include <memory>
#include <thread>

using nlohmann::json;
namespace {
struct Fixture {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("asterion-terminal-test-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Fixture() {
    EXPECT_TRUE((std::filesystem::create_directory(directory))) << "create temp directory";
  }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  auto write(std::string_view contents) {
    const auto file = directory / "trades.csv";
    std::ofstream out(file);
    out << contents;
    out.close();
    EXPECT_TRUE((static_cast<bool>(out))) << "write fixture";
    return file;
  }
};
json call(void* runtime, const json& request) {
  std::unique_ptr<char, decltype(&asterion_terminal_free)> result(
      asterion_terminal_call(runtime, request.dump().c_str()), asterion_terminal_free);
  EXPECT_TRUE((result != nullptr)) << "response allocated";
  return json::parse(result.get());
}
json request(std::string method, json params = json::object()) {
  return {{"version", 1}, {"method", method}, {"params", params}};
}
} // namespace
TEST(TerminalApi, Contracts) {

  std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(asterion_terminal_create(),
                                                                      asterion_terminal_destroy);
  EXPECT_TRUE((runtime != nullptr)) << "runtime allocated";
  EXPECT_TRUE((call(runtime.get(), request("runtime.snapshot"))["result"]["dataset"].is_null()))
      << "no invented dataset";
  {
    std::unique_ptr<char, decltype(&asterion_terminal_free)> response(
        asterion_terminal_call(
            runtime.get(),
            R"({"version":1,"method":"runtime.snapshot","method":"futures.inspect_csv","params":{}})"),
        asterion_terminal_free);
    EXPECT_TRUE((response && json::parse(response.get())["error"]["code"] == "invalid_request"))
        << "duplicate method cannot bypass dispatch";
  }
  Fixture fixture;
  const auto file = fixture.write("timestamp_ns,price,quantity\n100,3510,2\n200,3511,3\n");
  json params{{"path", file.string()},       {"venue", "SHFE"},
              {"symbol", "rb2610"},          {"product", "rb"},
              {"delivery_month", "2026-10"}, {"currency", "CNY"},
              {"price_increment", "1"},      {"quantity_increment", "1"},
              {"multiplier", "10"}};
  auto imported = call(runtime.get(), request("futures.inspect_csv", params));
  EXPECT_TRUE((imported.contains("result"))) << "valid futures CSV accepted";
  const auto dataset = imported["result"]["dataset"];
  EXPECT_TRUE(
      (dataset["count"] == 2 && dataset["quantity"] == "5" && dataset["last_price"] == "3511"))
      << "actual data summarized";
  EXPECT_TRUE((dataset["last_timestamp_ns"].is_string()))
      << "nanoseconds not exposed as JS numbers";
  for (const auto& [field, value] :
       std::vector<std::pair<std::string, std::string>>{{"delivery_month", "2026-13"},
                                                        {"symbol", "rbMAIN"},
                                                        {"symbol", "rb2611"},
                                                        {"quantity_increment", "0.5"},
                                                        {"multiplier", "0"},
                                                        {"venue", "UNKNOWN"}}) {
    auto bad = params;
    bad[field] = value;
    EXPECT_TRUE((call(runtime.get(), request("futures.inspect_csv", bad)).contains("error")))
        << "bad contract rejected";
  }
  fixture.write("timestamp_ns,price,quantity\n100,3510,2\n200,3511,0.5\n");
  EXPECT_TRUE((call(runtime.get(), request("futures.inspect_csv", params)).contains("error")))
      << "invalid row rejected";
  EXPECT_TRUE((call(runtime.get(), request("runtime.snapshot"))["result"]["dataset"] == dataset))
      << "failed import leaves whole previous snapshot intact";
  fixture.write("timestamp_ns,price,quantity\n");
  EXPECT_TRUE((call(runtime.get(),
                    request("futures.inspect_csv", params))["result"]["dataset"]["count"] == 0))
      << "header-only explicit empty";
  auto wrong = request("runtime.snapshot");
  wrong["version"] = 2;
  EXPECT_TRUE((call(runtime.get(), wrong).contains("error"))) << "unsupported version rejected";
  EXPECT_TRUE((call(runtime.get(), request("missing")).contains("error")))
      << "unsupported method rejected";
  EXPECT_TRUE((call(nullptr, request("runtime.snapshot")).contains("error")))
      << "null runtime rejected without crossing ABI";
  params["extra"] = true;
  EXPECT_TRUE((call(runtime.get(), request("futures.inspect_csv", params)).contains("error")))
      << "unknown fields rejected";
  using namespace asterion;
  FuturesContract czce{{{"CZCE", "SR609"},
                        AssetClass::futures,
                        "CNY",
                        Decimal::parse("1"),
                        Decimal::parse("1"),
                        Decimal::parse("10")},
                       "SR",
                       "2026-09"};
  czce.validate();
  czce.instrument.asset_class = AssetClass::equity;
  EXPECT_THROW(([&] { czce.validate(); })(), std::invalid_argument);
}

TEST(TerminalApi, PersistentPaperRoundTripThroughCAbi) {
  Fixture fixture;
  const auto file = fixture.write("timestamp_ns,price,quantity\n100,100,1\n200,99,1\n300,110,1\n");
  const auto directory = fixture.directory / "account";
  std::filesystem::create_directory(directory);
  auto make = [] {
    return std::unique_ptr<void, decltype(&asterion_terminal_destroy)>(asterion_terminal_create(),
                                                                       asterion_terminal_destroy);
  };
  auto runtime = make();
  auto invoke = [&](std::string method, json params = json::object()) {
    return call(runtime.get(), request(method, params));
  };
  json params{{"path", file.string()},       {"venue", "SHFE"},
              {"symbol", "rb2610"},          {"product", "rb"},
              {"delivery_month", "2026-10"}, {"currency", "CNY"},
              {"price_increment", "1"},      {"quantity_increment", "1"},
              {"multiplier", "10"}};
  ASSERT_TRUE(invoke("futures.inspect_csv", params).contains("result"));
  auto created = invoke("paper.create", {{"directory", directory.string()},
                                         {"deposit", "1000"},
                                         {"margin_per_lot", "100"},
                                         {"open_fee", "2"},
                                         {"close_today_fee", "3"},
                                         {"close_yesterday_fee", "4"},
                                         {"max_order_quantity", "100"},
                                         {"max_gross_quantity", "100"},
                                         {"max_working_orders", "100"}});
  ASSERT_TRUE(created.contains("result")) << created.dump();
  ASSERT_TRUE(
      invoke("paper.act", {{"request_id", "tick1"}, {"action", "advance"}}).contains("result"));
  ASSERT_TRUE(invoke("paper.act", {{"request_id", "buy"},
                                   {"action", "submit"},
                                   {"order_id", "o1"},
                                   {"side", "buy"},
                                   {"offset", "open"},
                                   {"quantity", "1"},
                                   {"price", "100"}})
                  .contains("result"));
  ASSERT_TRUE(
      invoke("paper.act", {{"request_id", "tick2"}, {"action", "advance"}}).contains("result"));
  const auto previous = invoke("runtime.snapshot")["result"]["paper"];
  runtime.reset();
  std::filesystem::remove(file);
  runtime = make();
  EXPECT_TRUE(invoke("runtime.snapshot")["result"]["paper"].is_null());
  auto recovered = invoke("paper.open", {{"directory", directory.string()}});
  ASSERT_TRUE(recovered.contains("result")) << recovered.dump();
  EXPECT_EQ(recovered["result"]["paper"], previous);
  EXPECT_EQ(
      invoke("paper.act", {{"request_id", "tick2"}, {"action", "advance"}})["result"]["paper"],
      previous);
  EXPECT_TRUE(
      invoke("paper.act", {{"request_id", "x"}, {"action", "live_order"}}).contains("error"));
  EXPECT_TRUE(invoke("paper.close")["result"]["paper"].is_null());
}
TEST(TerminalApi, StatusReadsDoNotQueueBehindLongOperations) {
  std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(asterion_terminal_create(),
                                                                      asterion_terminal_destroy);
  ASSERT_TRUE(runtime);
  const auto first = call(runtime.get(), request("runtime.snapshot"));
  ASSERT_TRUE(first.contains("result"));
  const auto revision = first["result"]["revision"].get<std::uint64_t>();
  EXPECT_GT(first["result"]["refreshed_at_ms"].get<std::int64_t>(), 0);
  // An unchanged revision returns no state at all.
  const auto same = call(runtime.get(), request("runtime.snapshot", {{"since", revision}}));
  EXPECT_EQ(same["result"]["unchanged"], true);
  EXPECT_EQ(same["result"]["revision"], revision);
  EXPECT_FALSE(same["result"].contains("dataset"));
  EXPECT_TRUE(call(runtime.get(), request("runtime.snapshot", {{"since", "x"}})).contains("error"));
  Fixture fixture;
  std::string csv = "timestamp_ns,price,quantity\n";
  for (int i = 1; i <= 200000; ++i)
    csv += std::to_string(i) + ",3510,1\n";
  const auto file = fixture.write(csv);
  json params{{"path", file.string()},       {"venue", "SHFE"},
              {"symbol", "rb2610"},          {"product", "rb"},
              {"delivery_month", "2026-10"}, {"currency", "CNY"},
              {"price_increment", "1"},      {"quantity_increment", "1"},
              {"multiplier", "10"}};
  std::atomic<bool> done{false};
  std::thread slow([&] {
    EXPECT_TRUE(call(runtime.get(), request("futures.inspect_csv", params)).contains("result"));
    done = true;
  });
  int concurrent_reads = 0;
  while (!done) {
    const auto started = std::chrono::steady_clock::now();
    const auto status = call(runtime.get(), request("runtime.snapshot"));
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              asterion::testing_support::bound(std::chrono::milliseconds(500)));
    if (!done) {
      ++concurrent_reads;
      EXPECT_TRUE(status["result"]["dataset"].is_null()) << "import not yet published";
    }
  }
  slow.join();
  EXPECT_GT(concurrent_reads, 0) << "import finished before a concurrent read was observed";
  const auto fresh = call(runtime.get(), request("runtime.snapshot", {{"since", revision}}));
  EXPECT_FALSE(fresh["result"].contains("unchanged"));
  EXPECT_GT(fresh["result"]["revision"].get<std::uint64_t>(), revision);
  EXPECT_EQ(fresh["result"]["dataset"]["count"], 200000);
}
