#include "../apps/clients/terminal/native/application_impl.hpp"
#include "timing.hpp"
#include <asterion/kernel/service_host.hpp>
#include <future>
#include <gtest/gtest.h>
using namespace asterion;
using namespace std::chrono_literals;
namespace asterion::terminal {
// Install a real IPC client without provisioning an Agent or touching user state.
struct ApplicationTestAccess {
  static void catalog(Application& app, std::int64_t cutoff) {
    std::lock_guard lock(app.impl_->operations);
    app.impl_->history_cutoff = cutoff;
    app.impl_->history_contracts = {{"CU2403.SHF",
                                     "copper",
                                     "SHFE",
                                     "CU",
                                     "20230101",
                                     "20240315",
                                     {},
                                     Decimal::parse("5.00000001"),
                                     "tonne",
                                     "CNY/tonne"}};
  }
  static void attach(Application& app, const ServiceEndpoint& endpoint) {
    auto client = std::make_shared<ResearchClient>(endpoint);
    std::lock_guard lock(app.impl_->operations);
    app.impl_->research = std::move(client);
    ++app.impl_->research_generation;
    ++app.impl_->mutations;
    app.impl_->publish(app.impl_->snapshot());
  }
};
} // namespace asterion::terminal
namespace {
struct MinuteService {
  std::filesystem::path root;
  std::string endpoint;
  std::mutex mutex;
  std::condition_variable condition;
  unsigned entered = 0;
  bool released = false;
  std::atomic<bool> wrong_identity{false};
  research::v1::TaskSubmit submitted;
  std::unique_ptr<service::ServiceHost> host;
  std::future<bool> running;
  MinuteService() {
    service::reset_stop_request();
#ifdef _WIN32
    root = std::filesystem::temp_directory_path() / ("ast-query-" + unique_process_id());
    endpoint = "asterion.query." + unique_process_id();
#else
    root = std::filesystem::path("/tmp") / ("ast-query-" + unique_process_id().substr(0, 12));
    endpoint = (root / "service").string();
#endif
    std::filesystem::create_directory(root);
    host = std::make_unique<service::ServiceHost>(
        service::Transport{endpoint, {}, 0, {}},
        [this](service::Connection& connection, std::stop_token) {
          research::v1::TaskRequest request;
          if (!request.ParseFromString(connection.receive(2s)))
            throw std::runtime_error("bad request");
          research::v1::TaskResponse response;
          response.set_version(1);
          response.set_service_id(request.service_id());
          response.set_correlation_id(request.correlation_id());
          if (request.has_list())
            response.mutable_tasks();
          else if (request.has_minute_page()) {
            {
              std::unique_lock lock(mutex);
              ++entered;
              condition.notify_all();
              if (!condition.wait_for(lock, 4s, [&] { return released; }))
                throw std::runtime_error("fixture query was not released");
            }
            const auto& query = request.minute_page();
            auto* page = response.mutable_minute_page();
            page->set_version(1);
            page->set_task_id(query.task_id());
            page->set_offset(query.offset());
            page->set_limit(query.limit());
            page->set_source("tushare.ft_mins");
            page->set_ts_code("CU2310.SHF");
            page->set_interval_minutes(1);
            page->set_manifest_sha256(std::string(64, 'a'));
            page->set_begin_ns(query.begin_ns() ? query.begin_ns() : 1);
            page->set_end_ns(query.end_ns() ? query.end_ns() : 2);
          } else if (request.has_submit()) {
            std::unique_lock lock(mutex);
            if (request.submit().has_daily_factor()) {
              ++entered;
              condition.notify_all();
              if (!condition.wait_for(lock, 4s, [&] { return released; }))
                throw std::runtime_error("fixture submission was not released");
            }
            submitted = request.submit();
            response.mutable_task()->set_id(submitted.id());
          } else if (request.has_daily_page()) {
            {
              std::unique_lock lock(mutex);
              ++entered;
              condition.notify_all();
              if (!condition.wait_for(lock, 4s, [&] { return released; }))
                throw std::runtime_error("fixture query was not released");
            }
            const auto& query = request.daily_page();
            auto* page = response.mutable_daily_page();
            page->set_version(1);
            page->set_task_id(wrong_identity ? "wrong" : query.task_id());
            page->set_offset(query.offset());
            page->set_limit(query.limit());
            page->set_source("tushare.fut_daily");
            page->set_ts_code("CU2403.SHF");
            page->set_manifest_sha256(std::string(64, 'a'));
            page->set_begin_day(query.begin_day().empty() ? "2023-01-01" : query.begin_day());
            page->set_end_day(query.end_day().empty() ? "2024-01-01" : query.end_day());
          } else
            throw std::runtime_error("unexpected fixture request");
          connection.send(response.SerializeAsString(), 2s);
        });
    running = std::async(std::launch::async, [&] { return host->run(); });
  }
  terminal::ServiceEndpoint address() const { return {{}, "minute-fixture", 0, {}, endpoint}; }
  bool wait(unsigned count) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, 1s, [&] { return entered >= count; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    condition.notify_all();
  }
  void reset() {
    std::lock_guard lock(mutex);
    entered = 0;
    released = false;
  }
  ~MinuteService() {
    release();
    service::request_stop();
    if (running.valid())
      running.wait();
    host.reset();
    std::error_code error;
    std::filesystem::remove_all(root, error);
    service::reset_stop_request();
  }
};
Json request(const std::string& method, Json params = Json::object()) {
  return {{"version", 1}, {"method", method}, {"params", std::move(params)}};
}
Json query(const std::string& id) {
  return request("research.minutes.page", {{"id", id},
                                           {"offset", 0},
                                           {"limit", 100},
                                           {"start", ""},
                                           {"end", ""},
                                           {"include_macd", true}});
}
} // namespace
TEST(TerminalMinuteQueries, ConcurrentReadersDoNotBlockCommandsOrPublishOldServicePages) {
  MinuteService source;
  MinuteService replacement;
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  auto first = std::async(std::launch::async, [&] { return app.dispatch(query("first")); });
  EXPECT_TRUE(source.wait(1));
  auto second = std::async(std::launch::async, [&] { return app.dispatch(query("second")); });
  EXPECT_TRUE(source.wait(2)) << "both chart reads must reach the service before either completes";
  const auto started = std::chrono::steady_clock::now();
  const auto snapshot = app.dispatch(request("runtime.snapshot"));
  EXPECT_FALSE(snapshot.value("stale", false));
  EXPECT_TRUE(snapshot.at("history_page").is_null());
  EXPECT_TRUE(app.dispatch(request("paper.close")).contains("protocol"));
  // This deliberately invalid mutation must reach validation, not fail as a busy command.
  EXPECT_THROW(app.dispatch(request("futures.inspect_csv")), std::invalid_argument);
  EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
  source.release();
  EXPECT_NO_THROW({ EXPECT_EQ(first.get().at("history_page").at("id"), "first"); });
  EXPECT_NO_THROW({ EXPECT_EQ(second.get().at("history_page").at("id"), "second"); });
  EXPECT_TRUE(app.dispatch(request("runtime.snapshot")).at("history_page").is_null());
  source.reset();
  auto obsolete = std::async(std::launch::async, [&] { return app.dispatch(query("obsolete")); });
  EXPECT_TRUE(source.wait(1));
  terminal::ApplicationTestAccess::attach(app, replacement.address());
  source.release();
  try {
    (void)obsolete.get();
    FAIL() << "old service reply was accepted";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::conflict);
    EXPECT_EQ(std::string(error.what()), "research service changed during minute query");
  }
  replacement.release();
  EXPECT_EQ(app.dispatch(query("current")).at("history_page").at("id"), "current");
  EXPECT_TRUE(app.dispatch(request("runtime.snapshot")).at("history_page").is_null());
}

TEST(TerminalMinuteQueries, OptionalTimeBoundsAreTypedAndDoNotRelaxOtherFields) {
  MinuteService source;
  source.release();
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  const auto empty = app.dispatch(query("unfiltered"));
  EXPECT_EQ(empty.at("history_page").at("begin_ns"), "1");
  auto filtered = query("filtered");
  filtered["params"]["start"] = "2023-08-25 09:00:00";
  filtered["params"]["end"] = "2023-08-25 10:00:00";
  const auto result = app.dispatch(filtered);
  EXPECT_EQ(result.at("history_page").at("begin_ns"),
            std::to_string(tushare::parse_time("2023-08-25 09:00:00")));
  for (const auto* key : {"start", "end"}) {
    auto invalid = query("invalid");
    invalid["params"][key] = 0;
    EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
    invalid["params"][key] = " ";
    EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
  }
  auto invalid = query("");
  EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
  invalid = query("invalid");
  invalid["params"]["limit"] = 201;
  EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
  invalid = query("invalid");
  invalid["params"]["unexpected"] = true;
  EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
}

TEST(TerminalDailyQueries, ConcurrentDailyAndMinuteReadersDoNotBlockOrPublishStalePages) {
  MinuteService source;
  MinuteService replacement;
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  auto daily = query("daily");
  daily["method"] = "research.daily.page";
  daily["params"]["period"] = "day";
  auto first = std::async(std::launch::async, [&] { return app.dispatch(daily); });
  EXPECT_TRUE(source.wait(1));
  auto minute = std::async(std::launch::async, [&] { return app.dispatch(query("minute")); });
  EXPECT_TRUE(source.wait(2));
  const auto before = std::chrono::steady_clock::now();
  const auto snapshot = app.dispatch(request("runtime.snapshot"));
  EXPECT_TRUE(snapshot.at("daily_page").is_null());
  EXPECT_TRUE(snapshot.at("history_page").is_null());
  EXPECT_TRUE(app.dispatch(request("paper.close")).contains("protocol"));
  EXPECT_LT(std::chrono::steady_clock::now() - before, testing_support::bound(500ms));
  source.release();
  EXPECT_EQ(first.get().at("daily_page").at("id"), "daily");
  EXPECT_EQ(minute.get().at("history_page").at("id"), "minute");
  EXPECT_TRUE(app.dispatch(request("runtime.snapshot")).at("daily_page").is_null());
  source.reset();
  auto obsolete = std::async(std::launch::async, [&] { return app.dispatch(daily); });
  EXPECT_TRUE(source.wait(1));
  terminal::ApplicationTestAccess::attach(app, replacement.address());
  source.release();
  try {
    (void)obsolete.get();
    FAIL() << "old service reply accepted";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::conflict);
    EXPECT_EQ(std::string(error.what()), "research service changed during daily query");
  }
  replacement.release();
  EXPECT_EQ(app.dispatch(daily).at("daily_page").at("id"), "daily");
  replacement.wrong_identity = true;
  EXPECT_THROW(app.dispatch(daily), Error);
}
TEST(TerminalDailyQueries, DatesAndParametersAreValidatedBeforeServiceIO) {
  MinuteService source;
  source.release();
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  auto daily = query("daily");
  daily["method"] = "research.daily.page";
  daily["params"]["period"] = "day";
  EXPECT_EQ(app.dispatch(daily).at("daily_page").at("begin_day"), "2023-01-01");
  daily["params"]["start"] = "2023-02-01";
  EXPECT_EQ(app.dispatch(daily).at("daily_page").at("begin_day"), "2023-02-01");
  for (const auto& change : std::vector<Json>{{{"period", "decade"}},
                                              {{"period", 0}},
                                              {{"start", 0}},
                                              {{"start", "2023-02-29"}},
                                              {{"start", "2023-01-01 09:00:00"}},
                                              {{"start", "2024-01-01"}, {"end", "2023-01-01"}},
                                              {{"id", "../daily"}},
                                              {{"offset", -1}},
                                              {{"offset", 99999}},
                                              {{"limit", 201}},
                                              {{"include_macd", 1}},
                                              {{"unknown", true}}}) {
    auto bad = daily;
    bad["params"].update(change);
    if (change.contains("id"))
      EXPECT_THROW(app.dispatch(bad), Error);
    else
      EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
  }
  {
    std::lock_guard lock(source.mutex);
    EXPECT_EQ(source.entered, 2);
  }
  // This fixture deliberately replies with DAY; reject a WEEK request's response.
  daily["params"]["period"] = "week";
  try {
    (void)app.dispatch(daily);
    FAIL() << "wrong aggregate period was accepted";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::unavailable);
    EXPECT_EQ(std::string(error.what()), "daily dataset page identity mismatch");
  }
}
TEST(TerminalDailyQueries, CatalogLifetimeUsesShanghaiDateAndStopsAtDelisting) {
  const tushare::FuturesListing item{"CU2403.SHF", "copper", "SHFE", "CU", "20230101", "20240315"};
  auto range = tushare::daily_contract_range(item, tushare::parse_time("2023-06-01 00:00:00"));
  EXPECT_EQ(format_trading_date(range.begin), "2023-01-01");
  EXPECT_EQ(format_trading_date(range.end), "2023-06-01");
  range = tushare::daily_contract_range(item, tushare::parse_time("2024-04-01 08:00:00"));
  EXPECT_EQ(format_trading_date(range.end), "2024-03-15");
  EXPECT_THROW(tushare::daily_contract_range(item, tushare::parse_time("2022-12-31 23:59:59")),
               std::invalid_argument);
}

TEST(TerminalDailyQueries, SubmissionRequiresCurrentCatalogAndSendsTypedDatesToService) {
  MinuteService source;
  source.release();
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  const auto cutoff = tushare::parse_time("2023-06-01 10:00:00");
  auto submission =
      request("research.daily.submit", {{"id", "daily"},
                                        {"ts_code", "CU2403.SHF"},
                                        {"requests_per_minute", 60},
                                        {"token", "fixture-secret"},
                                        {"catalog_cutoff_ns", std::to_string(cutoff)}});
  EXPECT_THROW(app.dispatch(submission), std::invalid_argument);
  terminal::ApplicationTestAccess::catalog(app, cutoff);
  const auto result = app.dispatch(submission);
  EXPECT_EQ(result.dump().find("fixture-secret"), std::string::npos);
  const auto& item = result.at("history_contracts").at("items").at(0);
  EXPECT_TRUE(item.at("multiplier").is_null());
  EXPECT_EQ(item.at("per_unit"), "5.00000001");
  EXPECT_EQ(item.at("trade_unit"), "tonne");
  EXPECT_EQ(item.at("quote_unit"), "CNY/tonne");
  {
    std::lock_guard lock(source.mutex);
    ASSERT_TRUE(source.submitted.has_daily());
    EXPECT_EQ(source.submitted.daily().begin_day(), "2023-01-01");
    EXPECT_EQ(source.submitted.daily().end_day(), "2023-06-01");
    EXPECT_EQ(source.submitted.provider_token(), "fixture-secret");
  }
  for (const auto& change : std::vector<Json>{{{"catalog_cutoff_ns", "0"}},
                                              {{"ts_code", "CU9999.SHF"}},
                                              {{"requests_per_minute", 501}},
                                              {{"requests_per_minute", 1.5}},
                                              {{"start", "2020-01-01"}}}) {
    auto bad = submission;
    bad["params"].update(change);
    EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
  }
}

TEST(TerminalDailyFactor, SubmissionDoesNotBlockOtherWindowsAndRejectsChangedService) {
  MinuteService source, replacement;
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  const auto first_id =
      app.dispatch(request("runtime.snapshot")).at("research").at("connection_id");
  EXPECT_TRUE(first_id.is_string());
  EXPECT_FALSE(first_id.get<std::string>().empty());
  EXPECT_EQ(app.dispatch(request("runtime.snapshot")).at("research").at("connection_id"), first_id);
  const auto submit =
      request("research.daily-factor.submit", {{"id", "analysis"},
                                               {"source_task_id", "daily"},
                                               {"lookback", 20},
                                               {"horizon", 5},
                                               {"evaluation", {{"mode", "full_sample"}}}});
  auto pending = std::async(std::launch::async, [&] { return app.dispatch(submit); });
  ASSERT_TRUE(source.wait(1));
  EXPECT_TRUE(app.dispatch(request("paper.close")).contains("protocol"));
  EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).value("stale", false));
  terminal::ApplicationTestAccess::attach(app, replacement.address());
  EXPECT_NE(app.dispatch(request("runtime.snapshot")).at("research").at("connection_id"), first_id);
  source.release();
  try {
    (void)pending.get();
    FAIL() << "obsolete service response must not be published";
  } catch (const Error& error) {
    EXPECT_EQ(
        std::string(error.what()),
        "research service changed during operation; inspect the original service before retrying");
  }
  replacement.release();
  EXPECT_TRUE(app.dispatch(submit).contains("protocol"));
  EXPECT_EQ(replacement.submitted.daily_factor().source_task_id(), "daily");
  EXPECT_EQ(replacement.submitted.daily_factor().lookback(), 20);
  auto bad = submit;
  bad["params"]["lookback"] = 1.5;
  EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
  bad = submit;
  bad["params"]["bars"] = Json::array();
  EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
}
