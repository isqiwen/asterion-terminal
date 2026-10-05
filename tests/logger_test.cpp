#include <ctime>
#include <chrono>
#include <cstdio>
#include <optional>
#include <cstdlib>
#include <gtest/gtest.h>
#include <asterion/kernel/logger.hpp>
#include <asterion/foundation/time.hpp>
#include <asterion/kernel/trace.hpp>
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
    throw std::runtime_error("test-only-sensitive-sink-detail");
  }
  void flush_() override { throw std::runtime_error("flush failed"); }
};
} // namespace
TEST(Logger, FailedSinkProducesPayloadFreeCountWithoutChangingTheBusinessResult) {
  auto logger = std::make_shared<Logger>(
      std::make_shared<spdlog::logger>("failed", std::make_shared<FailingSink>()));
  const auto before = process_log_failures();
  testing::internal::CaptureStderr();
  for (int i = 0; i < 20; ++i)
    EXPECT_FALSE(logger->write(LogLevel::info, "failed.probe", {{"message", "sensitive-request"}}));
  logger->flush();
  const auto notice = testing::internal::GetCapturedStderr();
  EXPECT_EQ(process_log_failures() - before, logger->failures());
  EXPECT_GE(logger->failures(), 21);
  EXPECT_EQ(notice.find("sensitive"), std::string::npos);
  EXPECT_EQ(notice.find("request"), std::string::npos);
  std::istringstream lines(notice);
  std::string line;
  unsigned emitted = 0;
  while (std::getline(lines, line)) {
    const auto value = Json::parse(line);
    EXPECT_EQ(value.size(), 3);
    EXPECT_EQ(value.at("event"), "logging.failed");
    const auto count = value.at("failures").get<std::uint64_t>();
    EXPECT_GT(count, before);
    EXPECT_EQ(count & (count - 1), 0);
    ++emitted;
  }
  EXPECT_GT(emitted, 0U);
  EXPECT_LT(emitted, 10U);
}
TEST_F(LogFixture, ProcessFailureEventsReuseTheSinkAndBoundRepeatedDiagnostics) {
  const char* value = std::getenv("ASTERION_LOG_DIRECTORY");
  const std::optional<std::string> saved = value ? std::optional<std::string>(value) : std::nullopt;
  struct Restore {
    const std::optional<std::string>& saved;
    ~Restore() {
      if (saved)
        ::setenv("ASTERION_LOG_DIRECTORY", saved->c_str(), 1);
      else
        ::unsetenv("ASTERION_LOG_DIRECTORY");
    }
  } restore{saved};
  ASSERT_EQ(::setenv("ASTERION_LOG_DIRECTORY", directory.c_str(), 1), 0);
  const auto logger = process_logger("bounded");
  EXPECT_EQ(logger, process_logger("bounded"));
  for (std::uint64_t i = 1; i <= 17; ++i)
    log_process_failure("bounded", "connection.failed", ErrorCode::unavailable, i);
  std::vector<unsigned> counts;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    std::ifstream stream(entry.path());
    std::string line;
    while (std::getline(stream, line)) {
      const auto record = Json::parse(line);
      EXPECT_EQ(record.at("fields").size(), 2);
      EXPECT_EQ(record.at("fields").at("code"), "unavailable");
      counts.push_back(record.at("fields").at("failures").get<unsigned>());
    }
  }
  EXPECT_EQ(counts, (std::vector<unsigned>{1, 2, 4, 8, 16}));
  const auto before = process_log_failures();
  ASSERT_EQ(::setenv("ASTERION_LOG_DIRECTORY", "relative", 1), 0);
  log_process_event("bounded", LogLevel::info, "creation.failed", {{"password", "test-secret"}});
  EXPECT_EQ(process_log_failures(), before + 1);
}

TEST(Logger, TraceScopesRestoreAcrossFailuresAndThreads) {
  const std::string outer = "test.outer";
  TraceScope scope(outer);
  auto fail = [] {
    const std::string nested = "test.nested";
    TraceScope child(nested);
    EXPECT_EQ(current_trace_id(), nested);
    const auto first = next_correlation_id(), second = next_correlation_id();
    EXPECT_NE(first, second);
    EXPECT_NO_THROW(validate_id(first));
    throw Error(ErrorCode::conflict, "test request failure");
  };
  EXPECT_THROW(fail(), Error);
  EXPECT_EQ(current_trace_id(), outer);
  std::jthread worker([&] {
    EXPECT_TRUE(current_trace_id().empty());
    EXPECT_THROW(fail(), Error);
    EXPECT_TRUE(current_trace_id().empty());
  });
  worker.join();
  EXPECT_EQ(current_trace_id(), outer);
}
