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
TEST_F(LogFixture, RotatingFilesRemainBounded) {
  LoggerOptions options;
  options.stderr_sink = false;
  options.file = directory / std::filesystem::path(std::u8string(u8"运行.log"));
  options.max_file_bytes = 512;
  options.retained_files = 2;
  {
    Logger logger(options);
    for (int i = 0; i < 100; ++i)
      ASSERT_TRUE(logger.write(LogLevel::info, "rotation.record", {{"sequence", i}}));
    logger.flush();
  }
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    ++count;
    EXPECT_LE(entry.file_size(), 512);
    std::ifstream input(entry.path());
    std::string line;
    while (std::getline(input, line))
      EXPECT_NO_THROW(static_cast<void>(Json::parse(line)));
  }
  EXPECT_EQ(count, 3);
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
