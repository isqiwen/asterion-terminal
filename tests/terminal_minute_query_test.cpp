#include "../apps/clients/terminal/native/application_impl.hpp"
#include "timing.hpp"
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/service_host.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <future>
#include <gtest/gtest.h>
using namespace asterion;
using namespace std::chrono_literals;
namespace asterion::terminal {
// Install a real IPC client without provisioning an Agent or touching user state.
struct ApplicationTestAccess {
  static void node(Application& app, std::shared_ptr<NodeClient> client) {
    std::lock_guard lock(app.impl_->operations);
    if (client)
      app.impl_->nodes["local"] = std::move(client);
    else
      app.impl_->nodes.erase("local");
  }
  static void catalog(Application& app, std::int64_t cutoff) {
    std::lock_guard lock(app.impl_->operations);
    app.impl_->history_cutoff = cutoff;
    app.impl_->history_source = "tushare.fut_daily";
    app.impl_->history_contracts = {{{"SHFE", "cu", "2024-03"},
                                     "copper",
                                     "2023-01-01",
                                     "2024-03-15",
                                     "CU2403.SHF",
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
  std::atomic<bool> wrong_identity{false}, reference{false};
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
          else if (request.has_history_usage()) {
            {
              std::unique_lock lock(mutex);
              ++entered;
              condition.notify_all();
              if (!condition.wait_for(lock, 4s, [&] { return released; }))
                throw std::runtime_error("fixture query was not released");
            }
            response.mutable_history_usage()->set_dataset_id(
                wrong_identity ? std::string(64, 'b') : request.history_usage().id());
            if (reference) {
              auto* row = response.mutable_history_usage()->add_references();
              row->set_kind(data::v1::HISTORY_BACKTEST);
              row->set_id("external-task");
              row->add_roles(data::v1::HISTORY_MARKET);
            }
          } else if (request.has_history_update_plan()) {
            {
              std::unique_lock lock(mutex);
              ++entered;
              condition.notify_all();
              if (!condition.wait_for(lock, 4s, [&] { return released; }))
                throw std::runtime_error("fixture query was not released");
            }
            auto* plan = response.mutable_history_update_plan();
            *plan->mutable_query() = request.history_update_plan();
            auto* input = plan->mutable_daily();
            input->set_source("tushare.fut_daily");
            input->set_source_instrument("CU2403.SHF");
            input->set_version(2);
            input->set_contract_id("SHFE/cu/2024-03");
            input->set_begin_day("2024-03-01");
            input->set_end_day(plan->query().end_day());
            input->set_requests_per_minute(plan->query().requests_per_minute());
            plan->set_id(sha256_bytes(plan->SerializeAsString()));
          } else if (request.has_minute_page()) {
            {
              std::unique_lock lock(mutex);
              ++entered;
              condition.notify_all();
              if (!condition.wait_for(lock, 4s, [&] { return released; }))
                throw std::runtime_error("fixture query was not released");
            }
            const auto& query = request.minute_page();
            auto* page = response.mutable_minute_page();
            page->set_version(2);
            page->set_task_id(query.task_id());
            page->set_offset(query.offset());
            page->set_limit(query.limit());
            page->set_source("tushare.ft_mins");
            page->set_contract_id("SHFE/cu/2023-10");
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
            page->set_version(2);
            page->set_task_id(wrong_identity ? "wrong" : query.task_id());
            page->set_offset(query.offset());
            page->set_limit(query.limit());
            page->set_source("tushare.fut_daily");
            page->set_contract_id("SHFE/cu/2024-03");
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
  EXPECT_TRUE(app.dispatch(request("live.close")).contains("protocol"));
  // This deliberately invalid mutation must reach validation, not fail as a busy command.
  EXPECT_THROW(app.dispatch(request("futures.inspect_csv")), std::exception);
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
            std::to_string(parse_shanghai_time("2023-08-25 09:00:00")));
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
  EXPECT_TRUE(app.dispatch(request("live.close")).contains("protocol"));
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
TEST(TerminalDailyQueries, SubmissionRequiresCurrentCatalogAndSendsTypedDatesToService) {
  MinuteService source;
  source.release();
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  const auto cutoff = parse_shanghai_time("2023-06-01 10:00:00");
  auto submission =
      request("research.daily.submit", {{"id", "daily"},
                                        {"connection", ""},
                                        {"connection_revision", ""},
                                        {"contract_id", "SHFE/cu/2024-03"},
                                        {"source", "tushare.fut_daily"},
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
                                              {{"contract_id", "CU9999.SHF"}},
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
                                               {"source_dataset_id", std::string(64, 'a')},
                                               {"lookback", 20},
                                               {"horizon", 5},
                                               {"evaluation", {{"mode", "full_sample"}}}});
  auto pending = std::async(std::launch::async, [&] { return app.dispatch(submit); });
  ASSERT_TRUE(source.wait(1));
  EXPECT_TRUE(app.dispatch(request("live.close")).contains("protocol"));
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
  EXPECT_EQ(replacement.submitted.daily_factor().source_dataset_id(), std::string(64, 'a'));
  EXPECT_EQ(replacement.submitted.daily_factor().lookback(), 20);
  auto bad = submit;
  bad["params"]["lookback"] = 1.5;
  EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
  bad = submit;
  bad["params"]["bars"] = Json::array();
  EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
}

TEST(TerminalHistoryUpdate, SlowPlanDoesNotBlockAndObsoleteServiceCannotSubmit) {
  MinuteService source;
  MinuteService replacement;
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  const Json params = {{"dataset_id", std::string(64, 'a')},
                       {"calendar_dataset_id", ""},
                       {"mode", "extend"},
                       {"end_day", "2024-03-02"},
                       {"requests_per_minute", 60}};
  auto pending = std::async(std::launch::async,
                            [&] { return app.dispatch(request("research.history.plan", params)); });
  EXPECT_TRUE(source.wait(1));
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).value("stale", false));
  EXPECT_TRUE(app.dispatch(request("live.close")).contains("protocol"));
  EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
  terminal::ApplicationTestAccess::attach(app, replacement.address());
  source.release();
  EXPECT_THROW((void)pending.get(), Error);
  replacement.release();
  const auto plan =
      app.dispatch(request("research.history.plan", params)).at("history_update_plan");
  auto submit = request("research.history.submit", {{"id", "update"},
                                                    {"query", params},
                                                    {"plan_id", plan.at("id")},
                                                    {"token", ""},
                                                    {"connection", ""},
                                                    {"connection_revision", ""}});
  replacement.reset();
  auto stale = std::async(std::launch::async, [&] { return app.dispatch(submit); });
  EXPECT_TRUE(replacement.wait(1));
  terminal::ApplicationTestAccess::attach(app, source.address());
  replacement.release();
  EXPECT_THROW((void)stale.get(), Error);
  EXPECT_TRUE(replacement.submitted.id().empty());
  submit["params"]["plan_id"] = std::string(64, '0');
  EXPECT_THROW(app.dispatch(submit), std::invalid_argument);
  EXPECT_TRUE(source.submitted.id().empty());
}

TEST(TerminalHistoryUsage, SlowQueryDoesNotBlockAndRejectsStaleOrWrongIdentity) {
  MinuteService source;
  MinuteService replacement;
  struct Environment {
    std::optional<std::string> previous = environment_variable("ASTERION_NODE_DIRECTORY");
    ~Environment() {
      if (previous)
        setenv("ASTERION_NODE_DIRECTORY", previous->c_str(), 1);
      else
        unsetenv("ASTERION_NODE_DIRECTORY");
    }
  } restore;
  setenv("ASTERION_NODE_DIRECTORY", source.root.c_str(), 1);
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  const auto query = request("research.history.usage", {{"id", std::string(64, 'a')}});
  auto pending = std::async(std::launch::async, [&] { return app.dispatch(query); });
  EXPECT_TRUE(source.wait(1));
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).value("stale", false));
  EXPECT_TRUE(app.dispatch(request("live.close")).contains("protocol"));
  EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
  terminal::ApplicationTestAccess::attach(app, replacement.address());
  source.release();
  EXPECT_THROW((void)pending.get(), Error);
  replacement.release();
  const auto result = app.dispatch(query).at("history_usage");
  EXPECT_EQ(result.at("dataset_id"), std::string(64, 'a'));
  EXPECT_TRUE(result.at("references").empty());
  EXPECT_TRUE(result.at("selected_roles").empty());
  replacement.wrong_identity = true;
  EXPECT_THROW(app.dispatch(query), Error);
}

TEST(TerminalHistoryUsage, OtherServicesAreScopedAndNodeChangesRejectObsoleteResults) {
  MinuteService source, other;
  struct Environment {
    std::optional<std::string> previous = environment_variable("ASTERION_NODE_DIRECTORY");
    ~Environment() {
      if (previous)
        setenv("ASTERION_NODE_DIRECTORY", previous->c_str(), 1);
      else
        unsetenv("ASTERION_NODE_DIRECTORY");
    }
  } restore;
  setenv("ASTERION_NODE_DIRECTORY", source.root.c_str(), 1);
  source.release();
  other.release();
  other.reference = true;
  std::atomic<unsigned> mutations{0};
  const auto socket = (source.root / "node.sock").string();
  service::ServiceHost host(
      {socket, {}, 0, {}}, [&](service::Connection& connection, std::stop_token) {
        node::v1::Request request;
        if (!request.ParseFromString(connection.receive(2s)) || !request.has_status()) {
          ++mutations;
          return;
        }
        node::v1::Response response;
        response.set_version(1);
        response.set_correlation_id(request.correlation_id());
        auto* status = response.mutable_status();
        status->set_instance_id("fixture");
        status->set_os("macos");
        status->set_arch("arm64");
        for (const auto& [name, endpoint, state] :
             std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"minute-fixture", source.endpoint, "running"},
                 {"other", other.endpoint, "running"},
                 {"stopped", other.endpoint + ".stopped", "stopped"},
                 {"offline", other.endpoint + ".missing", "running"}}) {
          auto* entry = status->add_services();
          entry->set_id(name);
          entry->set_endpoint(endpoint);
          entry->set_state(state);
          entry->set_kind(node::v1::TASK_SERVICE);
        }
        connection.send(response.SerializeAsString(), 2s);
      });
  auto running = std::async(std::launch::async, [&] { return host.run(); });
  struct Stop {
    std::future<bool>& running;
    ~Stop() {
      service::request_stop();
      running.wait();
    }
  } stop{running};
  auto node = std::make_shared<terminal::NodeClient>(
      terminal::NodeEndpoint{"local", "localhost", 0, {}, socket});
  terminal::Application app;
  terminal::ApplicationTestAccess::attach(app, source.address());
  auto current_address = source.address();
  current_address.host = "localhost"; // Host is irrelevant for the same IPC endpoint.
  terminal::ApplicationTestAccess::attach(app, current_address);
  terminal::ApplicationTestAccess::node(app, node);
  const auto enrollments = source.root / "enrollments";
  EXPECT_TRUE(terminal::registered_node_inventory().at("names").empty());
  EXPECT_FALSE(std::filesystem::exists(enrollments));
  std::filesystem::create_directories(enrollments / "offline");
  std::filesystem::create_directories(enrollments / ".ssh-keys");
  EXPECT_EQ(terminal::registered_node_inventory().at("names"), Json::array({"offline"}));
  const auto query = request("research.history.usage", {{"id", std::string(64, 'a')}});
  const auto queried = app.dispatch(query).at("history_usage");
  EXPECT_EQ(queried.at("disconnected_nodes").at("names"), Json::array({"offline"}));
  const auto groups = queried.at("other_research");
  ASSERT_EQ(groups.size(), 3U); // Current service is excluded, not inspected twice.
  EXPECT_TRUE(groups[0].at("checked"));
  EXPECT_EQ(groups[0].at("references")[0].at("id"), "external-task");
  EXPECT_FALSE(groups[1].at("checked"));
  EXPECT_EQ(groups[1].at("error"), "invalid local research ledger directory");
  EXPECT_FALSE(groups[2].at("checked"));
  other.wrong_identity = true;
  const auto wrong = app.dispatch(query).at("history_usage").at("other_research")[0];
  EXPECT_FALSE(wrong.at("checked"));
  EXPECT_TRUE(wrong.at("references").empty());
  other.wrong_identity = false;
  other.reset();
  auto pending = std::async(std::launch::async, [&] { return app.dispatch(query); });
  ASSERT_TRUE(other.wait(1));
  const auto begin = std::chrono::steady_clock::now();
  EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).value("stale", false));
  terminal::ApplicationTestAccess::node(app, nullptr);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, testing_support::bound(500ms));
  other.release();
  try {
    (void)pending.get();
    FAIL() << "stale node result accepted";
  } catch (const Error& error) {
    EXPECT_STREQ(error.what(), "node connections changed during archive query");
  }
  terminal::ApplicationTestAccess::node(app, node);
  other.reset();
  auto registry_pending = std::async(std::launch::async, [&] { return app.dispatch(query); });
  ASSERT_TRUE(other.wait(1));
  std::filesystem::create_directory(enrollments / "new-offline");
  other.release();
  try {
    (void)registry_pending.get();
    FAIL() << "stale registration result accepted";
  } catch (const Error& error) {
    EXPECT_STREQ(error.what(), "registered nodes changed during archive query");
  }
  std::filesystem::create_directory_symlink(source.root, enrollments / "linked");
  EXPECT_TRUE(terminal::registered_node_inventory().contains("error"));
  EXPECT_EQ(mutations, 0U);
}
