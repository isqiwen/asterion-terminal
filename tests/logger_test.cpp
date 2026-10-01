#include <ctime>
#include <chrono>
#include <cstdio>
#include <gtest/gtest.h>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/runtime.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/base_sink.h>
#include <fstream>
#include <sstream>
#include <thread>
using namespace asterion;
namespace {
struct LogFixture : testing::Test {
  std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("asterion-log-test-" + std::to_string(SystemClock{}.monotonic_now()));
  void SetUp() override { ASSERT_TRUE(std::filesystem::create_directory(directory)); }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};
} // namespace
TEST(Logger, FilteringRedactionAndConcurrentWrites) {
  std::ostringstream output;
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
  auto backend = std::make_shared<spdlog::logger>("test", sink);
  backend->set_level(spdlog::level::info);
  Logger logger(backend);
  EXPECT_TRUE(logger.write(LogLevel::debug, "hidden"));
  EXPECT_TRUE(output.str().empty());
  EXPECT_TRUE(logger.write(LogLevel::info, "session.open",
                           {{"password", "test-only-secret"},
                            {"nested", {{"AccessToken", "test-only-token"}}},
                            {"count", 2}}));
  std::vector<std::jthread> workers;
  for (int i = 0; i < 4; ++i)
    workers.emplace_back([&] {
      for (int j = 0; j < 50; ++j)
        EXPECT_TRUE(logger.write(LogLevel::info, "worker.tick"));
    });
  for (auto& worker : workers)
    worker.join();
  logger.flush();
  EXPECT_EQ(output.str().find("test-only-secret"), std::string::npos);
  EXPECT_EQ(output.str().find("test-only-token"), std::string::npos);
  std::istringstream lines(output.str());
  std::string line;
  int count = 0;
  while (std::getline(lines, line)) {
    EXPECT_TRUE(Json::parse(line).contains("timestamp_ns"));
    ++count;
  }
  EXPECT_EQ(count, 201);
  EXPECT_FALSE(logger.write(LogLevel::info, "invalid event"));
  EXPECT_FALSE(logger.write(LogLevel::info, "oversized", {{"value", std::string(17000, 'x')}}));
  EXPECT_EQ(logger.failures(), 2);
}
TEST_F(LogFixture, DailyFilesKeepThirtyDaysAndLeaveOtherFiles) {
  // Dated files from earlier days: one inside the window, one outside it.
  const auto dated = [&](int days_ago) {
    // Local dates, as the daily sink names its files.
    const auto when = std::time(nullptr) - static_cast<std::time_t>(days_ago) * 86400;
    std::tm local{};
    localtime_r(&when, &local);
    char name[32];
    std::strftime(name, sizeof name, "service_%Y-%m-%d.log", &local);
    return directory / name;
  };
  std::ofstream(dated(29)) << "recent\n";
  std::ofstream(dated(45)) << "expired\n";
  std::ofstream(directory / "notes.log") << "other\n";
  LoggerOptions options;
  options.stderr_sink = false;
  options.file = directory / "service.log";
  {
    Logger logger(options);
    ASSERT_TRUE(logger.write(LogLevel::info, "daily.record", {{"sequence", 1}}));
    logger.flush();
  }
  EXPECT_TRUE(std::filesystem::exists(dated(0)));
  EXPECT_TRUE(std::filesystem::exists(dated(29)));
  EXPECT_FALSE(std::filesystem::exists(dated(45)));
  EXPECT_TRUE(std::filesystem::exists(directory / "notes.log"));
  std::ifstream input(dated(0));
  std::string line;
  std::getline(input, line);
  EXPECT_EQ(Json::parse(line).at("event"), "daily.record");
  options.file = "relative.log";
  EXPECT_THROW(Logger{options}, Error);
}
namespace {
class FailingSink final : public spdlog::sinks::base_sink<std::mutex> {
  void sink_it_(const spdlog::details::log_msg&) override {
    throw std::runtime_error("sink failed");
  }
  void flush_() override { throw std::runtime_error("flush failed"); }
};
} // namespace
TEST(Logger, SinkFailureDoesNotChangeCommandResult) {
  auto logger = std::make_shared<Logger>(
      std::make_shared<spdlog::logger>("failed", std::make_shared<FailingSink>()));
  EXPECT_FALSE(logger->write(LogLevel::info, "failure.probe"));
  Runtime runtime("logging.test", std::make_shared<ManualClock>(), logger);
  runtime.access().grant("local", "read");
  runtime.command("read", "read", [](const Json&) { return Json(42); });
  runtime.start();
  EXPECT_EQ(runtime.dispatch("local", "read", Json::object()), 42);
  runtime.stop();
  EXPECT_GE(logger->failures(), 4);
}
