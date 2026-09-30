#include "../apps/clients/terminal/native/market_history.hpp"
#include "timing.hpp"
#include "history_fixture.hpp"
#include <gtest/gtest.h>
#include <asterion/terminal.h>
#include <asterion/domain/futures.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
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
json history(void* runtime, const std::vector<int>& prices, const std::string& id) {
  auto invoke = [&](const std::string& method, json params = json::object()) {
    auto result = call(runtime, request(method, std::move(params)));
    if (result.contains("error"))
      throw std::runtime_error(result.dump());
    return result.at("result");
  };
  invoke("research.local");
  const auto stopped =
      invoke("node.action", {{"id", "local"}, {"service", "research"}, {"action", "stop"}});
  std::string directory;
  for (const auto& node : stopped.at("nodes"))
    if (node.at("id") == "local")
      for (const auto& service : node.at("health").at("services"))
        if (service.at("id") == "research")
          directory = service.at("directory").get<std::string>();
  auto selection = asterion::test::seed_history(directory, prices, id);
  invoke("research.local");
  return selection;
}
} // namespace
TEST(TerminalApi, SnapshotRejectsMalformedQueriesBeforeReadingState) {
  std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(asterion_terminal_create(),
                                                                      asterion_terminal_destroy);
  ASSERT_NE(runtime, nullptr);
  for (const auto& params : std::vector<json>{nullptr,
                                              json::array(),
                                              true,
                                              "invalid",
                                              {{"extra", true}},
                                              {{"since", -1}},
                                              {{"since", 1.5}},
                                              {{"since", "1"}},
                                              {{"since", 0}, {"extra", true}}}) {
    const auto response = call(runtime.get(), request("runtime.snapshot", params));
    ASSERT_TRUE(response.contains("error")) << params.dump() << ": " << response.dump();
    EXPECT_EQ(response.at("error").at("code"), "invalid_request");
  }
  EXPECT_TRUE(call(runtime.get(), request("runtime.snapshot")).contains("result"));
}
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
  const auto params = history(runtime.get(), {100, 101}, "contracts");
  auto imported = call(runtime.get(), request("research.dataset.select", params));
  ASSERT_TRUE(imported.contains("result")) << imported.dump();
  const auto dataset = imported["result"]["dataset"];
  EXPECT_EQ(dataset["count"], 2);
  EXPECT_EQ(dataset["last_close"], "101");
  EXPECT_TRUE(dataset["last_timestamp_ns"].is_string());
  auto invalid = params;
  invalid["price_increment"] = "0";
  EXPECT_TRUE(call(runtime.get(), request("research.dataset.select", invalid)).contains("error"));
  EXPECT_EQ(call(runtime.get(), request("runtime.snapshot"))["result"]["dataset"], dataset);
  EXPECT_TRUE(call(runtime.get(), request("futures.inspect_csv", {{"path", "removed.csv"}}))
                  .contains("error"));
  auto wrong = request("runtime.snapshot");
  wrong["version"] = 2;
  EXPECT_TRUE((call(runtime.get(), wrong).contains("error"))) << "unsupported version rejected";
  EXPECT_TRUE((call(runtime.get(), request("missing")).contains("error")))
      << "unsupported method rejected";
  EXPECT_TRUE((call(nullptr, request("runtime.snapshot")).contains("error")))
      << "null runtime rejected without crossing ABI";
  auto extra = params;
  extra["extra"] = true;
  EXPECT_TRUE((call(runtime.get(), request("research.dataset.select", extra)).contains("error")))
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
  const auto params = history(runtime.get(), {100, 99, 110}, "paper-roundtrip");
  ASSERT_TRUE(invoke("research.dataset.select", params).contains("result"));
  auto created = invoke("paper.create", {{"directory", directory.string()},
                                         {"deposit", "1000"},
                                         {"margin_per_lot", "100"},
                                         {"open_fee", "2"},
                                         {"close_today_fee", "3"},
                                         {"close_yesterday_fee", "4"},
                                         {"margin_rate", "0"},
                                         {"open_fee_rate", "0"},
                                         {"close_today_fee_rate", "0"},
                                         {"close_yesterday_fee_rate", "0"},
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
  auto revision = first["result"]["revision"].get<std::uint64_t>();
  EXPECT_GT(first["result"]["refreshed_at_ms"].get<std::int64_t>(), 0);
  // The probe's own call count is published by the next background refresh,
  // so wait for the state to settle; then an unchanged revision carries no state.
  json same;
  for (int attempt = 0; attempt < 5; ++attempt) {
    same = call(runtime.get(), request("runtime.snapshot", {{"since", revision}}));
    if (same["result"].value("unchanged", false))
      break;
    revision = same["result"]["revision"].get<std::uint64_t>();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  }
  EXPECT_EQ(same["result"]["unchanged"], true);
  EXPECT_EQ(same["result"]["revision"], revision);
  EXPECT_FALSE(same["result"].contains("dataset"));
  EXPECT_TRUE(call(runtime.get(), request("runtime.snapshot", {{"since", "x"}})).contains("error"));
  const auto params = history(runtime.get(), std::vector<int>(500, 100), "concurrent");
  ASSERT_TRUE(call(runtime.get(), request("research.dataset.clear")).contains("result"));
  std::atomic<bool> done{false};
  std::thread slow([&] {
    EXPECT_TRUE(call(runtime.get(), request("research.dataset.select", params)).contains("result"));
    done = true;
  });
  int concurrent_reads = 0;
  int busy_commands = 0;
  while (!done) {
    const auto invalid = call(runtime.get(), request("runtime.snapshot", {{"extra", true}}));
    EXPECT_TRUE(invalid.contains("error"));
    if (invalid.contains("error"))
      EXPECT_EQ(invalid.at("error").at("code"), "invalid_request");
    const auto started = std::chrono::steady_clock::now();
    const auto status = call(runtime.get(), request("runtime.snapshot"));
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              asterion::testing_support::bound(std::chrono::milliseconds(500)));
    // The import may publish between this read and the done flag; either way
    // a reader sees no dataset or the complete one, never a partial state.
    const auto& dataset = status["result"]["dataset"];
    if (dataset.is_null())
      ++concurrent_reads;
    else
      EXPECT_EQ(dataset["count"], 500);
    if (status["result"].value("stale", false)) {
      const auto command_started = std::chrono::steady_clock::now();
      const auto inspect = call(runtime.get(), request("node.agent.inspect"));
      EXPECT_LT(std::chrono::steady_clock::now() - command_started,
                asterion::testing_support::bound(std::chrono::milliseconds(500)));
      if (inspect.contains("error")) {
        EXPECT_EQ(inspect["error"]["code"], "conflict");
        ++busy_commands;
      } else {
        EXPECT_TRUE(inspect.contains("result")); // Import completed before this call.
      }
    }
  }
  slow.join();
  EXPECT_GT(concurrent_reads, 0) << "import finished before a concurrent read was observed";
  EXPECT_GT(busy_commands, 0) << "commands must not queue behind a running import";
  const auto fresh = call(runtime.get(), request("runtime.snapshot", {{"since", revision}}));
  EXPECT_FALSE(fresh["result"].contains("unchanged"));
  EXPECT_GT(fresh["result"]["revision"].get<std::uint64_t>(), revision);
  EXPECT_EQ(fresh["result"]["dataset"]["count"], 500);
}

TEST(MarketHistory, BoundsEventsAndBreaksOnGapsFailuresAndDisconnects) {
  asterion::terminal::MarketHistory history;
  asterion::market::v1::EventBatch batch;
  batch.set_stream_id("fixture-stream");
  batch.set_latest_sequence(520);
  for (std::uint64_t i = 1; i <= 520; ++i) {
    auto* event = batch.add_events();
    event->set_sequence(i);
    auto* quote = event->mutable_quote()->mutable_quote();
    quote->mutable_instrument()->set_venue("SHFE");
    quote->mutable_instrument()->set_symbol("rb2610");
    quote->set_last("3510.00000001");
    quote->set_source_ms(1790582400000 + static_cast<std::int64_t>(i));
  }
  history.append(batch);
  auto view = history.snapshot();
  ASSERT_EQ(view.at("points").size(), 512);
  EXPECT_EQ(view.at("points").front().at("price"), "3510.00000001");
  EXPECT_EQ(view.at("points").front().at("timestamp_ns"), "1790582400009000000");
  EXPECT_EQ(history.cursor, 520);
  batch.clear_events();
  batch.set_latest_sequence(530);
  batch.set_gap(true);
  auto* event = batch.add_events();
  event->set_sequence(530);
  *event->mutable_quote()->mutable_quote() = asterion::market::v1::Quote{};
  history.append(batch);
  EXPECT_TRUE(history.snapshot().at("points").empty());
  EXPECT_TRUE(history.snapshot().at("interrupted"));
  batch.clear_events();
  batch.set_gap(false);
  batch.set_failed(true);
  history.append(batch);
  EXPECT_FALSE(history.snapshot().at("available"));
  history.interrupt();
  EXPECT_EQ(history.cursor, 0);
  EXPECT_TRUE(history.stream.empty());
  batch.set_stream_id("restarted");
  batch.set_failed(false);
  batch.set_latest_sequence(1);
  batch.add_events()->set_sequence(1);
  batch.mutable_events(0)->mutable_status()->set_phase("disconnected");
  history.append(batch);
  EXPECT_TRUE(history.snapshot().at("points").empty());
}
TEST(MarketHistory, RejectsBrokenOrderAndSkipsOutOfOrderQuotes) {
  asterion::terminal::MarketHistory history;
  asterion::market::v1::EventBatch batch;
  batch.set_stream_id("fixture-stream");
  batch.set_latest_sequence(2);
  batch.add_events()->set_sequence(2);
  EXPECT_THROW(history.append(batch), std::invalid_argument);
  EXPECT_EQ(history.cursor, 0);
  batch.mutable_events(0)->set_sequence(1);
  auto* observation = batch.mutable_events(0)->mutable_quote();
  observation->set_out_of_order(true);
  observation->mutable_quote()->set_last("100");
  observation->mutable_quote()->set_source_ms(1000);
  history.append(batch);
  EXPECT_EQ(history.cursor, 1);
  EXPECT_TRUE(history.snapshot().at("points").empty());
  EXPECT_THROW(history.append(batch), std::invalid_argument);
}

TEST(MarketHistory, VolumeUsesPerContractDayBaselineAndResetsOnDiscontinuity) {
  asterion::terminal::MarketHistory history;
  std::uint64_t sequence = 0;
  const auto append = [&](const char* symbol, const char* day, std::int64_t volume,
                          bool gap = false, bool out_of_order = false) {
    asterion::market::v1::EventBatch batch;
    batch.set_stream_id("volume-fixture");
    batch.set_latest_sequence(++sequence);
    batch.set_gap(gap);
    auto* event = batch.add_events();
    event->set_sequence(sequence);
    auto* observation = event->mutable_quote();
    observation->set_out_of_order(out_of_order);
    auto* quote = observation->mutable_quote();
    quote->mutable_instrument()->set_venue("SHFE");
    quote->mutable_instrument()->set_symbol(symbol);
    quote->set_trading_day(day);
    quote->set_source_ms(1000 + static_cast<std::int64_t>(sequence));
    quote->set_last("100");
    quote->set_volume(volume);
    history.append(batch);
  };
  append("rb", "20260929", 1000);
  EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
  append("cu", "20260929", 9000);
  append("rb", "20260929", 1007);
  EXPECT_EQ(history.snapshot()["points"].back()["volume"], "7");
  append("rb", "20260929", 1007);
  EXPECT_EQ(history.snapshot()["points"].back()["volume"], "0");
  append("rb", "20260929", 3000, false, true);
  append("rb", "20260929", 1010);
  EXPECT_EQ(history.snapshot()["points"].back()["volume"], "3");
  append("rb", "20260930", 50);
  EXPECT_EQ(history.snapshot()["points"].size(), 2); // cu is retained.
  EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
  append("rb", "20260930", 40);
  EXPECT_EQ(history.snapshot()["points"].size(), 2);
  EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
  append("rb", "20260930", 45);
  EXPECT_EQ(history.snapshot()["points"].back()["volume"], "5");
  append("rb", "20260930", 100, true);
  EXPECT_EQ(history.snapshot()["points"].size(), 1);
  EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
  append("rb", "", 110);
  append("rb", "", 120);
  EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
  append("rb", "20260930", -1);
  append("rb", "20260930", 130);
  EXPECT_FALSE(history.snapshot()["points"].back().contains("volume"));
}

int main(int argc, char** argv) {
  // Direct invocation must never register test sessions in the user's daily Agent.
  const char* directory = std::getenv("ASTERION_NODE_DIRECTORY");
  const char* isolated = std::getenv("ASTERION_TEST_NODE_ISOLATED");
  if (!directory || !*directory || !isolated || std::string_view(isolated) != "1") {
    std::cerr << "Run terminal tests through CTest or tests/isolated_node.py; "
                 "an isolated test Agent is required.\n";
    return 2;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
TEST(TerminalApi, LargeResearchDatasetsBacktestButPaperSessionsStaySmall) {
  std::unique_ptr<void, decltype(&asterion_terminal_destroy)> runtime(asterion_terminal_create(),
                                                                      asterion_terminal_destroy);
  auto invoke = [&](std::string method, json params = json::object()) {
    return call(runtime.get(), request(method, std::move(params)));
  };
  // Above the paper-session limit, within the research limit.
  std::vector<int> prices(30000);
  for (std::size_t i = 0; i < prices.size(); ++i)
    prices[i] = 3000 + static_cast<int>(i % 200);
  const auto selected = invoke("research.dataset.select", history(runtime.get(), prices, "large"));
  ASSERT_TRUE(selected.contains("result")) << selected.dump().substr(0, 400);
  EXPECT_EQ(selected["result"]["dataset"]["count"], 30000);
  const json costs{{"deposit", "1000000"},
                   {"margin_per_lot", "100"},
                   {"open_fee", "2"},
                   {"close_today_fee", "3"},
                   {"close_yesterday_fee", "4"},
                   {"margin_rate", "0"},
                   {"open_fee_rate", "0"},
                   {"close_today_fee_rate", "0"},
                   {"close_yesterday_fee_rate", "0"},
                   {"max_order_quantity", "10"},
                   {"max_gross_quantity", "10"},
                   {"max_working_orders", "10"}};
  auto backtest = costs;
  backtest.update({{"id", "large-backtest"}, {"fast", 5}, {"slow", 20}, {"quantity", "1"}});
  const auto submitted = invoke("research.submit", backtest);
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump().substr(0, 400);
  auto paper = costs;
  paper["directory"] = std::filesystem::temp_directory_path().string();
  const auto refused = invoke("paper.create", paper);
  ASSERT_TRUE(refused.contains("error"));
  EXPECT_NE(refused["error"]["message"].get<std::string>().find("at most 20000 bars"),
            std::string::npos);
}
