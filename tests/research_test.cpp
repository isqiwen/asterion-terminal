#include "sqlite_database.hpp"
#include "engine.hpp"
#include "risk_module.hpp"
#include "bar_fixture.hpp"
#include "moving_average.hpp"
#include "task_store.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
Decimal d(const char* text) {
  return Decimal::parse(text);
}
research::v1::BacktestInput input() {
  std::vector<MarketBar> bars;
  std::int64_t time = 1790298000000000000LL;
  for (const auto price : {100, 101, 102, 101, 100, 101, 103}) {
    bars.push_back(test::flat("2026-09-25", time, std::to_string(price).c_str(), "10"));
    time += 1000000000;
  }
  const auto dataset = test::dataset(bars, {{"2026-09-25", d("103")}});
  const auto manifest = Json{{"version", 3},
                             {"type", "historical_paper"},
                             {"risk",
                              {{"max_order_quantity", "100"},
                               {"max_gross_quantity", "100"},
                               {"max_working_orders", std::uint64_t{100}}}},
                             {"deposit", "10000"},
                             {"contracts",
                              {{{"dataset", protocol::decode_bar_dataset(dataset)},
                                {"costs",
                                 {{"margin_per_lot", "100"},
                                  {"open_fee", "2"},
                                  {"close_today_fee", "3"},
                                  {"close_yesterday_fee", "4"},
                                  {"margin_rate", "0"},
                                  {"open_fee_rate", "0"},
                                  {"close_today_fee_rate", "0"},
                                  {"close_yesterday_fee_rate", "0"}}}}}}};
  research::v1::BacktestInput result;
  result.set_version(7);
  *result.mutable_paper() = protocol::encode_input(manifest);
  result.mutable_sma()->set_fast(1);
  result.mutable_sma()->set_slow(3);
  result.mutable_sma()->mutable_quantity()->set_units(d("1").raw());
  result.set_dataset_revision(protocol::dataset_revision(result.paper()));
  return result;
}
} // namespace
TEST(Research, HashUsesExactSnapshotAndExcludesExperimentCosts) {
  EXPECT_EQ(sha256_bytes("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto spec = input();
  const auto revision = spec.dataset_revision();
  spec.mutable_paper()->mutable_deposit()->set_units(d("20000").raw());
  EXPECT_EQ(protocol::dataset_revision(spec.paper()), revision);
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_bars(0)
      ->mutable_close()
      ->set_units(d("99").raw());
  EXPECT_THROW(protocol::dataset_revision(spec.paper()), std::invalid_argument);
  EXPECT_THROW(backtest::validate(spec), std::invalid_argument);
}
TEST(Research, DeterministicNextTickExecutionFeesAndDrawdown) {
  const auto spec = input();
  const auto result = backtest::run(spec);
  EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
  ASSERT_EQ(result.account().fills_size(), 2);
  EXPECT_EQ(result.account().fills(0).price().units(), d("101").raw());
  EXPECT_EQ(result.account().fills(1).price().units(), d("101").raw());
  EXPECT_EQ(result.account().fees().units(), d("5").raw());
  EXPECT_EQ(result.account().equity().units(), d("9995").raw());
  EXPECT_EQ(result.max_drawdown().units(), d("12").raw());
  ASSERT_EQ(result.equity_size(), 8);
  EXPECT_EQ(result.equity(2).equity().units(), d("10000").raw());
  EXPECT_EQ(result.equity(3).equity().units(), d("9998").raw());
  // A sell limit at 101 must not fill on the following 100 trade.
  EXPECT_EQ(result.equity(4).equity().units(), d("9988").raw());
  EXPECT_EQ(result.account().frozen().units(), 0);
  EXPECT_EQ(result.account().positions_size(), 0);
}
TEST(Research, RejectsUnsupportedDaysAndStopsCooperatively) {
  auto spec = input();
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars(6)->set_trading_day(
      "2026-09-26");
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars(6)->set_timestamp_ns(
      spec.paper().contracts(0).dataset().bars(6).timestamp_ns() + 86400LL * 1000000000);
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = input();
  spec.mutable_sma()->set_fast(3);
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = input();
  std::stop_source stop;
  std::size_t done = 0;
  EXPECT_THROW(backtest::run(spec, stop.get_token(),
                             [&](auto n, auto) {
                               done = n;
                               stop.request_stop();
                             }),
               std::runtime_error);
  EXPECT_EQ(done, 1U);
}
TEST(Research, SmaWarmupAndLifecycleUseSamePluginOutsideBacktest) {
  Instrument instrument{{"SHFE", "rb2610"}, AssetClass::futures, "CNY", d("1"), d("1"), d("10")};
  MovingAverage strategy(instrument, 1, 3, d("1"));
  auto tick = test::flat("2026-09-25", 100, "100", "1");
  EXPECT_THROW(strategy.on_bar(tick), std::logic_error);
  strategy.start();
  EXPECT_FALSE(strategy.on_bar(tick));
  tick.timestamp_ns++;
  tick.open = tick.high = tick.low = tick.close = d("101");
  EXPECT_FALSE(strategy.on_bar(tick));
  tick.timestamp_ns++;
  tick.open = tick.high = tick.low = tick.close = d("102");
  EXPECT_EQ(strategy.on_bar(tick), d("1"));
  tick.timestamp_ns--;
  EXPECT_THROW(strategy.on_bar(tick), std::invalid_argument);
  strategy.stop();
  strategy.start();
  EXPECT_FALSE(strategy.on_bar(tick));
}
TEST(Research, InputRoundTripRejectsUnknownOrMissingFields) {
  const auto spec = input();
  const auto json = protocol::decode_backtest(spec);
  EXPECT_EQ(protocol::encode_backtest(json).SerializeAsString(), spec.SerializeAsString());
  auto bad = json;
  bad["unknown"] = true;
  EXPECT_THROW(protocol::encode_backtest(bad), Error);
  auto old = spec;
  old.set_version(1);
  EXPECT_THROW(protocol::decode_backtest(old), std::invalid_argument);
  auto missing = spec;
  missing.mutable_sma()->clear_quantity();
  EXPECT_THROW(protocol::decode_backtest(missing), std::invalid_argument);
}

namespace {
struct TaskDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("asterion-research-" + unique_process_id());
  TaskDirectory() { std::filesystem::create_directory(path); }
  ~TaskDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
} // namespace
TEST(ResearchTasks, DuplicateSubmissionAndStaleAttemptsAreFenced) {
  TaskDirectory directory;
  tasks::Store store(directory.path);
  auto spec = input();
  const auto first = store.submit("job1", spec);
  EXPECT_EQ(store.submit("job1", spec).SerializeAsString(), first.SerializeAsString());
  spec.mutable_sma()->mutable_quantity()->set_units(d("2").raw());
  EXPECT_THROW(store.submit("job1", spec), std::invalid_argument);
  const auto old = store.claim("job1");
  store.progress("job1", old, 2);
  EXPECT_THROW(store.progress("job1", old, 1), std::invalid_argument);
  EXPECT_EQ(store.cancel("job1").state(), research::v1::CANCEL_REQUESTED);
  store.finish("job1", old, backtest::run(input()));
  EXPECT_EQ(store.get("job1").state(), research::v1::CANCELLED);
  EXPECT_THROW(store.result("job1"), std::invalid_argument);
  store.retry("job1");
  const auto next = store.claim("job1");
  EXPECT_NE(old, next);
  EXPECT_THROW(store.finish("job1", old, backtest::run(input())), std::invalid_argument);
  store.finish("job1", next, backtest::run(input()));
  EXPECT_EQ(store.get("job1").attempt(), 2U);
  EXPECT_EQ(store.get("job1").state(), research::v1::SUCCEEDED);
  EXPECT_EQ(store.result("job1").SerializeAsString(), backtest::run(input()).SerializeAsString());
  EXPECT_THROW(store.retry("job1"), std::invalid_argument);
}
TEST(ResearchTasks, DispatchOwnsSubmissionOrderCapacityAndDoesNotClaimBeforeLaunch) {
  TaskDirectory directory;
  tasks::Store store(directory.path);
  for (const auto* id : {"z-first", "a-second", "m-third"})
    store.submit(id, input());
  research::v1::TaskDispatch processes;
  auto launches = store.dispatch(processes);
  ASSERT_EQ(launches.launches_size(), 2);
  EXPECT_EQ(launches.launches(0).task_id(), "z-first");
  EXPECT_EQ(launches.launches(1).task_id(), "a-second");
  EXPECT_EQ(launches.launches(0).program(), research::v1::BACKTEST_PROGRAM);

  EXPECT_EQ(store.get("z-first").state(), research::v1::QUEUED);
  EXPECT_EQ(store.get("z-first").attempt(), 0U);
  // An OS process may be running before it claims its attempt.
  processes.add_running("z-first");
  launches = store.dispatch(processes);
  ASSERT_EQ(launches.launches_size(), 1);
  EXPECT_EQ(launches.launches(0).task_id(), "a-second");
  processes.add_running("a-second");
  EXPECT_EQ(store.dispatch(processes).launches_size(), 0);
  processes.clear_running();
  store.cancel("z-first");
  const auto token = store.claim("a-second");
  launches = store.dispatch(processes);
  ASSERT_EQ(launches.launches_size(), 1);
  EXPECT_EQ(launches.launches(0).task_id(), "m-third");
  store.interrupt("a-second", token, "test worker exited");
  EXPECT_EQ(store.dispatch(processes).launches_size(), 1);
  store.retry("a-second");
  launches = store.dispatch(processes);
  EXPECT_EQ(launches.launches(0).task_id(), "a-second");
  processes.add_running("a-second");
  processes.add_running("a-second");
  EXPECT_THROW(store.dispatch(processes), std::invalid_argument);
}
TEST(ResearchTasks, RestartRetainsQueueAndResultsButInterruptsUnconfirmedWork) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path);
    store.submit("queued", input());
    store.submit("working", input());
    store.submit("done", input());
    const auto token = store.claim("working");
    store.progress("working", token, 2);
    store.finish("done", store.claim("done"), backtest::run(input()));
    EXPECT_THROW(tasks::Store other(directory.path), std::runtime_error);
  }
  {
    tasks::Store restored(directory.path);
    EXPECT_EQ(restored.get("queued").state(), research::v1::QUEUED);
    EXPECT_EQ(restored.get("working").state(), research::v1::INTERRUPTED);
    EXPECT_EQ(restored.get("working").completed(), 2U);
    EXPECT_EQ(restored.get("done").state(), research::v1::SUCCEEDED);
    EXPECT_EQ(restored.result("done").SerializeAsString(),
              backtest::run(input()).SerializeAsString());
    restored.retry("working");
    EXPECT_EQ(restored.get("working").state(), research::v1::QUEUED);
  }
  tasks::Store again(directory.path);
  EXPECT_EQ(again.get("working").state(), research::v1::QUEUED);
}
TEST(ResearchTasks, ModifiedResultNeverLoadsAsSuccess) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path);
    store.submit("done", input());
    store.finish("done", store.claim("done"), backtest::run(input()));
  }
  {
    std::ofstream file(directory.path / "done" / "results" / "1.json", std::ios::app);
    file << " ";
  }
  EXPECT_THROW(tasks::Store corrupted(directory.path), std::invalid_argument);
}
TEST(ResearchTasks, VerificationSnapshotCannotBypassCancellationOrANewerAttempt) {
  for (const bool retry : {false, true}) {
    SCOPED_TRACE(retry);
    TaskDirectory directory;
    tasks::Store store(directory.path);
    store.submit("job", input());
    research::v1::TaskFinish request;
    request.set_id("job");
    request.set_token(store.claim("job"));
    *request.mutable_result() = backtest::run(input());
    auto pending = store.prepare_finish(request);
    EXPECT_THROW(store.finish(pending), std::invalid_argument);
    // The host can service other operations while verification owns only copies.
    store.progress("job", request.token(), 1);
    store.cancel("job");
    if (retry) {
      store.acknowledge_cancel("job", request.token());
      store.retry("job");
      EXPECT_NE(store.claim("job"), request.token());
    }
    pending.verify();
    if (retry) {
      EXPECT_THROW(store.finish(std::move(pending)), std::invalid_argument);
      EXPECT_EQ(store.get("job").state(), research::v1::RUNNING);
      EXPECT_EQ(store.get("job").attempt(), 2U);
    } else {
      store.finish(std::move(pending));
      EXPECT_EQ(store.get("job").state(), research::v1::CANCELLED);
    }
    EXPECT_FALSE(std::filesystem::exists(directory.path / "job" / "results" / "1.json"));
  }
}
TEST(ResearchTasks, VerificationSnapshotIsOwnedAndExpiredAttemptCannotCommit) {
  TaskDirectory directory;
  tasks::Store store(directory.path);
  store.submit("job", input());
  research::v1::TaskFinish request;
  request.set_id("job");
  request.set_token(store.claim("job"));
  *request.mutable_result() = backtest::run(input());
  auto pending = store.prepare_finish(request);
  request.mutable_result()->set_dataset_revision("changed.after.prepare");
  EXPECT_NO_THROW(pending.verify());
  store.interrupt("job", request.token(), "test lease expired");
  EXPECT_THROW(store.finish(std::move(pending)), std::invalid_argument);
  EXPECT_EQ(store.get("job").state(), research::v1::INTERRUPTED);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "job" / "results" / "1.json"));
}
TEST(ResearchTasks, RepeatedVerifiedReadsStillDetectChangedResultBytes) {
  TaskDirectory directory;
  tasks::Store store(directory.path);
  store.submit("done", input());
  const auto expected = backtest::run(input());
  store.finish("done", store.claim("done"), expected);
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(store.result("done").SerializeAsString(), expected.SerializeAsString());
  {
    std::ofstream file(directory.path / "done" / "results" / "1.json", std::ios::app);
    file << " ";
  }
  EXPECT_THROW(store.result("done"), std::invalid_argument);
}
TEST(ResearchTasks, InvalidInputAndQueuedCancellationDoNotRunAnything) {
  TaskDirectory directory;
  tasks::Store store(directory.path);
  auto spec = input();
  spec.set_dataset_revision("invalid");
  EXPECT_THROW(store.submit("invalid", spec), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "invalid"));
  EXPECT_THROW(store.submit("../outside", input()), std::exception);
  store.submit("queued", input());
  EXPECT_EQ(store.cancel("queued").state(), research::v1::CANCELLED);
  EXPECT_THROW(store.claim("queued"), std::invalid_argument);
  EXPECT_EQ(store.get("queued").attempt(), 0U);
}

#include <asterion/kernel/ipc/local_channel.hpp>
#include <thread>
namespace {
using namespace std::chrono_literals;
namespace research_wire = research::v1;
struct ResearchProcess : testing::Test {
  TaskDirectory directory;
  std::unique_ptr<ChildProcess> service;
  std::filesystem::path sockets;
  std::string endpoint;
  void SetUp() override {
#ifdef _WIN32
    endpoint = "asterion.research." + unique_process_id();
#else
    sockets = std::filesystem::path("/tmp") / ("ast-r-" + unique_process_id().substr(0, 12));
    std::filesystem::create_directory(sockets);
    std::filesystem::permissions(sockets, std::filesystem::perms::owner_all);
    endpoint = (sockets / "task.sock").string();
#endif
    start();
  }
  void TearDown() override {
    service.reset();
    if (!sockets.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(sockets, ec);
    }
  }
  research_wire::TaskResponse call(research_wire::TaskRequest request,
                                   const std::string& target = "") {
    request.set_version(1);
    request.set_service_id("research");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(target.empty() ? endpoint : target, 2s);
    channel.send(request.SerializeAsString(), 2s);
    research_wire::TaskResponse response;
    if (!response.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("bad research response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != request.service_id() ||
        response.correlation_id() != request.correlation_id())
      throw std::runtime_error("wrong research response identity");
    if (response.has_error())
      throw std::runtime_error(response.error().message());
    return response;
  }
  void start() {
#ifndef _WIN32
    std::filesystem::remove(endpoint); // This test exclusively owns the private directory.
    std::filesystem::remove(endpoint + ".worker");
    std::filesystem::remove(endpoint + ".health");
#endif
    const auto directory_utf8 = directory.path.u8string();
    service = std::make_unique<ChildProcess>(
        ASTERION_TASK_SERVICE_PATH,
        std::vector<std::string>{
            "--directory", std::string(directory_utf8.begin(), directory_utf8.end()), "--endpoint",
            endpoint, "--worker-endpoint", endpoint + ".worker", "--health-endpoint",
            endpoint + ".health", "--session", "research", "--worker-timeout", "2"});
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        research_wire::TaskRequest req;
        req.mutable_heartbeat();
        call(req);
        break;
      } catch (const std::exception&) {
        if (service->exited() || std::chrono::steady_clock::now() > deadline)
          throw;
        std::this_thread::sleep_for(20ms);
      }
    }
  }
  void submit(const std::string& id) {
    service.reset();
    {
      tasks::Store store(directory.path);
      store.submit(id, input());
    }
    start();
  }
};
} // namespace
TEST_F(ResearchProcess, WorkerRunsWithoutSubmittingClientAndResultSurvivesRestart) {
  submit("run1"); // Every request closes its connection; no UI client remains.
  ChildProcess worker(ASTERION_BACKTEST_PATH,
                      {"--endpoint", endpoint, "--session", "research", "--task", "run1"});
  ASSERT_TRUE(worker.wait(15s));
  ASSERT_EQ(worker.exit_code(), 0);
  research_wire::TaskRequest result;
  result.mutable_result()->set_id("run1");
  EXPECT_EQ(call(result).backtest().SerializeAsString(),
            backtest::run(input()).SerializeAsString());
  research_wire::TaskRequest list;
  list.mutable_list();
  auto response = call(list);
  ASSERT_EQ(response.tasks().tasks_size(), 1);
  EXPECT_FALSE(response.tasks().tasks(0).has_input());
  service.reset();
  start();
  EXPECT_EQ(call(result).backtest().SerializeAsString(),
            backtest::run(input()).SerializeAsString());
  submit("run1"); // Same task ID after restart remains idempotent.
  EXPECT_EQ(call(list).tasks().tasks_size(), 1);
}
TEST_F(ResearchProcess, RestartFencesClaimedWorkAndQueuedCancelStaysCancelled) {
  submit("interrupted");
  research_wire::TaskRequest claim;
  claim.mutable_claim()->set_kind(research::v1::BACKTEST);
  claim.mutable_claim()->set_id("interrupted");
  const auto token = call(claim).attempt().token();
  service.reset();
  start();
  research_wire::TaskRequest get;
  get.mutable_get()->set_id("interrupted");
  EXPECT_EQ(call(get).task().state(), research_wire::INTERRUPTED);
  research_wire::TaskRequest finish;
  auto* f = finish.mutable_finish();
  f->set_id("interrupted");
  f->set_token(token);
  *f->mutable_result() = backtest::run(input());
  EXPECT_THROW(call(finish), std::runtime_error);
  submit("cancelled");
  research_wire::TaskRequest cancel;
  cancel.mutable_cancel()->set_id("cancelled");
  EXPECT_EQ(call(cancel).task().state(), research_wire::CANCELLED);
  ChildProcess worker(ASTERION_BACKTEST_PATH,
                      {"--endpoint", endpoint, "--session", "research", "--task", "cancelled"});
  ASSERT_TRUE(worker.wait(10s));
  EXPECT_NE(worker.exit_code(), 0);
}
TEST_F(ResearchProcess, ListenerServesRepeatedConnectionsAndRetainsOwnership) {
  for (int i = 0; i < 5; ++i) {
    research_wire::TaskRequest request;
    request.mutable_heartbeat();
    EXPECT_TRUE(call(request).has_health());
  }
  EXPECT_THROW(ipc::Listener duplicate(endpoint), Error);
}

TEST_F(ResearchProcess, WorkerLeaseExpiryInterruptsWithoutAutomaticRetry) {
  submit("lost");
  research_wire::TaskRequest claim;
  claim.mutable_claim()->set_kind(research::v1::BACKTEST);
  claim.mutable_claim()->set_id("lost");
  const auto token = call(claim).attempt().token();
  research_wire::TaskRequest get;
  get.mutable_get()->set_id("lost");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (call(get).task().state() == research_wire::RUNNING &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(50ms);
  EXPECT_EQ(call(get).task().state(), research_wire::INTERRUPTED);
  EXPECT_EQ(call(get).task().attempt(), 1U);
  research_wire::TaskRequest progress;
  progress.mutable_progress()->set_id("lost");
  progress.mutable_progress()->set_token(token);
  progress.mutable_progress()->set_completed(1);
  EXPECT_THROW(call(progress), std::runtime_error);
}

TEST(ResearchTasks, RejectsMismatchedResultContractAndMetrics) {
  TaskDirectory directory;
  tasks::Store store(directory.path);
  store.submit("job", input());
  const auto token = store.claim("job");
  auto result = backtest::run(input());
  result.mutable_account()->mutable_contracts(0)->mutable_contract()->set_symbol("rb2611");
  EXPECT_THROW(store.finish("job", token, result), std::invalid_argument);
  result = backtest::run(input());
  result.mutable_max_drawdown()->set_units(0);
  EXPECT_THROW(store.finish("job", token, result), std::invalid_argument);
  EXPECT_EQ(store.get("job").state(), research::v1::RUNNING);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "job" / "results" / "1.json"));
  store.finish("job", token, backtest::run(input()));
}

TEST(ResearchTasks, RejectsResultWithDifferentRiskConfiguration) {
  TaskDirectory directory;
  tasks::Store store(directory.path);
  const auto spec = input();
  store.submit("risk-result", spec);
  const auto token = store.claim("risk-result");
  auto result = backtest::run(spec);
  result.mutable_account()->mutable_risk()->set_max_working_orders(999);
  EXPECT_THROW(store.finish("risk-result", token, result), std::invalid_argument);
  EXPECT_EQ(store.get("risk-result").state(), research::v1::RUNNING);
  store.finish("risk-result", token, backtest::run(spec));
  EXPECT_EQ(store.result("risk-result").account().risk().SerializeAsString(),
            spec.paper().risk().SerializeAsString());
}

namespace {
struct TaskClock final : Clock {
  Nanoseconds value = 1790298000000000000LL;
  Nanoseconds utc_now() const override { return value; }
  Nanoseconds monotonic_now() const override { return 0; }
};
} // namespace
TEST(ResearchTasks, SubmissionOrderSurvivesEqualTimesClockRollbackAndRestart) {
  TaskDirectory directory;
  auto clock = std::make_shared<TaskClock>();
  research::v1::Task first, latest;
  {
    tasks::Store store(directory.path, clock);
    first = store.submit("z-first", input());
    store.submit("a-second", input()); // Same millisecond, opposite ID order.
    clock->value -= 60000000000LL;
    latest = store.submit("m-latest", input());
    EXPECT_LT(latest.submitted_at_ms(), first.submitted_at_ms());
    ASSERT_EQ(store.list().tasks_size(), 3);
    EXPECT_EQ(store.list().tasks(0).id(), "z-first");
    EXPECT_EQ(store.list().tasks(1).id(), "a-second");
    EXPECT_EQ(store.list().tasks(2).id(), "m-latest");
    EXPECT_EQ(store.submit("z-first", input()).SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(latest.submission_sequence(), 3U);
  }
  tasks::Store restored(directory.path, clock);
  EXPECT_EQ(restored.get("z-first").SerializeAsString(), first.SerializeAsString());
  EXPECT_EQ(restored.get("m-latest").SerializeAsString(), latest.SerializeAsString());
  EXPECT_EQ(restored.list().tasks(2).id(), "m-latest");
  EXPECT_EQ(restored.submit("b-next", input()).submission_sequence(), 4U);
  EXPECT_EQ(restored.list().tasks(3).id(), "b-next");
}
TEST(ResearchTasks, DurableUpdatesDoNotChangeSubmissionIdentityOrReorderRetries) {
  TaskDirectory directory;
  auto clock = std::make_shared<TaskClock>();
  research::v1::Task retried;
  {
    tasks::Store store(directory.path, clock);
    const auto original = store.submit("first", input());
    store.submit("second", input());
    clock->value += 1000000000;
    const auto token = store.claim("first");
    EXPECT_EQ(store.get("first").updated_at_ms(), clock->value / 1000000);
    clock->value += 1000000000;
    store.progress("first", token, 1);
    const auto progress = store.get("first");
    clock->value += 1000000000;
    store.progress("first", token, 1); // Repeated progress is not a new event.
    EXPECT_EQ(store.get("first").updated_at_ms(), progress.updated_at_ms());
    store.fail("first", token, "test worker failure");
    clock->value += 1000000000;
    retried = store.retry("first");
    EXPECT_EQ(retried.updated_at_ms(), clock->value / 1000000);
    EXPECT_EQ(retried.SerializeAsString(), store.get("first").SerializeAsString());
    EXPECT_EQ(retried.submitted_at_ms(), original.submitted_at_ms());
    EXPECT_EQ(retried.submission_sequence(), original.submission_sequence());
    EXPECT_EQ(store.list().tasks(0).id(), "first");
    auto json = protocol::decode_task(retried);
    EXPECT_EQ(json.at("submitted_at_ms"), original.submitted_at_ms());
    EXPECT_EQ(json.at("updated_at_ms"), retried.updated_at_ms());
    auto missing = retried;
    missing.clear_submitted_at_ms();
    EXPECT_THROW(protocol::decode_task(missing), std::invalid_argument);
  }
  tasks::Store restored(directory.path, clock);
  EXPECT_EQ(restored.get("first").SerializeAsString(), retried.SerializeAsString());
}
TEST(ResearchTasks, RejectsMissingOrDuplicateChronologyWithoutRewritingEvidence) {
  for (const auto& mode : {"old", "missing", "duplicate", "gap", "negative", "overflow"}) {
    SCOPED_TRACE(mode);
    TaskDirectory directory;
    {
      tasks::Store store(directory.path);
      store.submit("first", input());
      store.submit("second", input());
    }
    const auto manifest = [&] {
      sqlite::Database database(directory.path / "tasks.sqlite");
      sqlite::Database::Statement read(database, "SELECT manifest FROM tasks WHERE id='second'");
      EXPECT_TRUE(read.step());
      return Json::parse(read.text(0));
    };
    auto record = manifest();
    const std::string kind = mode;
    if (kind == "old")
      record["version"] = 1;
    else if (kind == "missing")
      record.erase("submitted_at_ms");
    else if (kind == "duplicate")
      record["submission_sequence"] = 1;
    else if (kind == "gap")
      record["submission_sequence"] = 3;
    else if (kind == "negative")
      record["submitted_at_ms"] = -1;
    else
      record["submitted_at_ms"] = std::numeric_limits<std::uint64_t>::max();
    {
      sqlite::Database database(directory.path / "tasks.sqlite");
      sqlite::Database::Statement write(database, "UPDATE tasks SET manifest=? WHERE id='second'");
      write.bind(1, record.dump()).step();
    }
    EXPECT_THROW(tasks::Store rejected(directory.path), std::exception);
    EXPECT_EQ(manifest(), record);
  }
}

TEST_F(ResearchProcess, SilentExternalPeerDoesNotBlockQueriesAndCancellation) {
  submit("cancel-me");
  auto silent = ipc::Channel::connect(endpoint, 2s);
  research_wire::TaskRequest cancel;
  cancel.mutable_cancel()->set_id("cancel-me");
  EXPECT_EQ(call(cancel).task().state(), research_wire::CANCELLED);
}
TEST_F(ResearchProcess, SilentWorkerPeerDoesNotBlockWorkerControl) {
  submit("claim-me");
  auto silent = ipc::Channel::connect(endpoint + ".worker", 2s);
  research_wire::TaskRequest claim;
  claim.mutable_claim()->set_id("claim-me");
  claim.mutable_claim()->set_kind(research_wire::BACKTEST);
  EXPECT_EQ(call(claim, endpoint + ".worker").attempt().task().state(), research_wire::RUNNING);
}
TEST_F(ResearchProcess, ExternalSaturationPreservesWorkerControlAndLeaseExpiry) {
  submit("expired");
  research_wire::TaskRequest claim;
  claim.mutable_claim()->set_id("expired");
  claim.mutable_claim()->set_kind(research_wire::BACKTEST);
  const auto token = call(claim, endpoint + ".worker").attempt().token();
  std::vector<ipc::Channel> silent;
  for (unsigned i = 0; i < 20; ++i)
    silent.push_back(ipc::Channel::connect(endpoint, 2s));
  research_wire::TaskRequest get;
  get.mutable_get()->set_id("expired");
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  while (call(get, endpoint + ".worker").task().state() == research_wire::RUNNING &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(30ms);
  EXPECT_EQ(call(get, endpoint + ".worker").task().state(), research_wire::INTERRUPTED);
  research_wire::TaskRequest progress;
  progress.mutable_progress()->set_id("expired");
  progress.mutable_progress()->set_token(token);
  progress.mutable_progress()->set_completed(1);
  EXPECT_THROW(call(progress, endpoint + ".worker"), std::runtime_error);
}

TEST(ResearchTasks, CompletedResultCarriesPersistedExperimentAndRejectsMismatchedEvidence) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path);
    store.submit("evidence", input());
    store.finish("evidence", store.claim("evidence"), backtest::run(input()));
  }
  tasks::Store restored(directory.path);
  research::v1::TaskResponse response;
  *response.mutable_backtest() = restored.result("evidence");
  *response.mutable_result_task() = restored.get("evidence");
  const auto value = protocol::decode_task_result(response, "evidence");
  EXPECT_EQ(value.at("experiment").at("sma"), Json({{"fast", 1}, {"slow", 3}, {"quantity", "1"}}));
  EXPECT_EQ(value.at("experiment").at("paper").at("deposit"), "10000");
  EXPECT_EQ(
      value.at("experiment").at("paper").at("contracts").at(0).at("costs").at("close_today_fee"),
      "3");
  EXPECT_EQ(value.at("experiment").at("paper").at("risk").at("max_order_quantity"), "100");
  EXPECT_FALSE(
      value.at("experiment").at("paper").at("contracts").at(0).at("dataset").contains("bars"));
  EXPECT_EQ(value.at("experiment").at("data").at(0).at("count"), 7);
  EXPECT_EQ(value.at("task").at("result_digest"), restored.get("evidence").result_digest());
  EXPECT_THROW(protocol::decode_task_result(response, "other"), std::invalid_argument);
  auto bad = response;
  bad.clear_result_task();
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_result_task()->set_state(research::v1::RUNNING);
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_backtest()->set_dataset_revision(std::string(64, '0'));
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_result_task()->clear_definition();
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_result_task()->set_kind(research::v1::FACTOR);
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
}

TEST(Research, ExplicitTradingDaysDoNotInferFromWallClock) {
  auto spec = input();
  auto* data = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
  data->mutable_bars(0)->set_timestamp_ns(1790254800000000000LL);
  data->set_revision(protocol::bar_dataset_revision(*data));
  spec.set_dataset_revision(data->revision());
  EXPECT_NO_THROW(backtest::run(spec));
  data->mutable_bars(0)->set_trading_day("2026-09-24");
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}

namespace {
research::v1::BacktestInput multiday_input() {
  auto spec = input();
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->clear_bars();
  const std::int64_t first = 1790298000000000000LL;
  const std::int64_t next = first + 3LL * 86400 * 1000000000;
  int index = 0;
  for (const auto price : {100, 101, 102, 101, 104, 103, 102, 103}) {
    auto* tick = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->add_bars();
    tick->set_trading_day(index < 4 ? "2026-09-25" : "2026-09-28");
    tick->set_timestamp_ns((index < 4 ? first : next) + (index % 4) * 1000000000LL);
    for (auto* p :
         {tick->mutable_open(), tick->mutable_high(), tick->mutable_low(), tick->mutable_close()})
      p->set_units(d(std::to_string(price).c_str()).raw());
    tick->mutable_volume()->set_units(d("10").raw());
    ++index;
  }
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(0)
      ->mutable_settlement_price()
      ->set_units(d("105").raw());
  auto* day = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->add_days();
  *day = spec.paper().contracts(0).dataset().days(0);
  day->set_trading_day("2026-09-28");
  day->mutable_settlement_price()->set_units(d("110").raw());
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  return spec;
}
} // namespace
TEST(Research, MultidaySettlementCarriesSmaAndChargesYesterdayClose) {
  const auto spec = multiday_input();
  const auto result = backtest::run(spec);
  ASSERT_EQ(result.settlements_size(), 2);
  EXPECT_EQ(result.settlements(0).contracts(0).price().units(), d("105").raw());
  EXPECT_EQ(result.settlements(0).equity().units(), d("10038").raw());
  EXPECT_EQ(result.settlements(0).contracts(0).position_quantity().units(), d("1").raw());
  EXPECT_EQ(result.settlements(1).contracts(0).position_quantity().units(), 0);
  EXPECT_EQ(result.account().balance().units(), d("10014").raw());
  EXPECT_EQ(result.account().fees().units(), d("6").raw());
  EXPECT_EQ(result.account().realized().units(), d("20").raw());
  EXPECT_EQ(result.max_drawdown().units(), d("30").raw());
  ASSERT_EQ(result.account().orders_size(), 2);
  EXPECT_EQ(result.account().orders(1).offset(), protocol::v1::CLOSE_YESTERDAY);
  ASSERT_EQ(result.equity_size(), 10);
  EXPECT_EQ(result.equity(4).event(), research::v1::DAILY_SETTLEMENT);
  EXPECT_EQ(result.equity(4).timestamp_ns(),
            spec.paper().contracts(0).dataset().bars(3).timestamp_ns());
  EXPECT_EQ(result.equity(5).event(), research::v1::TRADE_MARK);
  EXPECT_EQ(result.equity(9).equity().units(), result.account().equity().units());
  EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
}
TEST(Research, FinalSettlementRevaluesOpenPositionAndParticipatesInDrawdown) {
  auto spec = multiday_input();
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_days()->DeleteSubrange(1,
                                                                                                1);
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars()->DeleteSubrange(4,
                                                                                                4);
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(0)
      ->mutable_settlement_price()
      ->set_units(d("90").raw());
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  const auto result = backtest::run(spec);
  EXPECT_EQ(result.account().equity().units(), d("9888").raw());
  EXPECT_EQ(result.account().realized().units(), d("-110").raw());
  EXPECT_EQ(result.account().unrealized().units(), 0);
  EXPECT_EQ(result.max_drawdown().units(), d("112").raw());
  ASSERT_EQ(result.account().positions_size(), 1);
  EXPECT_EQ(result.account().positions(0).basis().units(), d("90").raw());
  EXPECT_EQ(result.equity(3).equity().units(), d("9998").raw());
  EXPECT_EQ(result.equity(4).event(), research::v1::DAILY_SETTLEMENT);
}
TEST(Research, MultidayRequiresExplicitValidCompleteEvidence) {
  const auto original = multiday_input();
  EXPECT_EQ(protocol::encode_backtest(protocol::decode_backtest(original)).SerializeAsString(),
            original.SerializeAsString());
  auto spec = original;
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(1)
      ->clear_settlement_price();
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = original;
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(1)
      ->mutable_settlement_price()
      ->set_units(d("-1").raw());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);

  spec = original;
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(1)
      ->mutable_settlement_price()
      ->set_units(d("110.5").raw());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = original;
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_days(1)->set_trading_day(
      spec.paper().contracts(0).dataset().days(0).trading_day());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);

  spec = original;
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars()->DeleteSubrange(4,
                                                                                                4);
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = original;
  spec.set_version(3);
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  auto json = protocol::decode_backtest(original);
  json["paper"]["contracts"][0]["dataset"]["days"][0]["settlement_price"] = "106";
  EXPECT_THROW(protocol::encode_backtest(json), std::invalid_argument);
}
TEST(ResearchTasks, MultidayEvidenceRestoresAndForgedSettlementCannotCommit) {
  TaskDirectory root;
  const auto spec = multiday_input();
  const auto expected = backtest::run(spec);
  auto altered = spec;
  altered.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(0)
      ->mutable_settlement_price()
      ->set_units(d("107").raw());
  altered.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(altered.paper().contracts(0).dataset()));
  altered.set_dataset_revision(altered.paper().contracts(0).dataset().revision());
  const auto wrong = backtest::run(altered);
  EXPECT_EQ(wrong.account().equity().units(), expected.account().equity().units());
  {
    tasks::Store store(root.path);
    store.submit("multiday", spec);
    const auto token = store.claim("multiday");
    EXPECT_THROW(store.finish("multiday", token, wrong), std::invalid_argument);
    auto forged = expected;
    forged.mutable_settlements(0)->mutable_contracts(0)->mutable_price()->set_units(d("106").raw());
    EXPECT_THROW(store.finish("multiday", token, forged), std::invalid_argument);
    store.finish("multiday", token, expected);
  }
  tasks::Store restored(root.path);
  EXPECT_EQ(restored.result("multiday").SerializeAsString(), expected.SerializeAsString());
  EXPECT_EQ(restored.get("multiday").input().SerializeAsString(), spec.SerializeAsString());
}

TEST_F(ResearchProcess, UpgradeFreezesAdmissionsWaitsForCompletionAndPreservesQueuedWork) {
  submit("active");
  submit("queued");
  research_wire::TaskRequest claim;
  claim.mutable_claim()->set_id("active");
  claim.mutable_claim()->set_kind(research_wire::BACKTEST);
  const auto attempt = call(claim, endpoint + ".worker").attempt();
  research_wire::TaskRequest freeze;
  freeze.mutable_quiesce();
  EXPECT_THROW(call(freeze), std::runtime_error);
  EXPECT_TRUE(call(freeze, endpoint + ".health").has_health());
  research_wire::TaskRequest blocked;
  blocked.mutable_submit()->set_id("blocked");
  EXPECT_THROW(call(blocked), std::runtime_error);
  research_wire::TaskRequest stop;
  stop.mutable_quiesce()->set_stop(true);
  EXPECT_THROW(call(stop, endpoint + ".health"), std::runtime_error);
  ASSERT_FALSE(service->exited());
  research_wire::TaskRequest finish;
  finish.mutable_finish()->set_id("active");
  finish.mutable_finish()->set_token(attempt.token());
  *finish.mutable_finish()->mutable_result() = backtest::run(input());
  call(finish, endpoint + ".worker");
  EXPECT_TRUE(call(stop, endpoint + ".health").has_health());
  ASSERT_TRUE(service->wait(15s));
  EXPECT_EQ(service->exit_code(), 0);
  start();
  research_wire::TaskRequest get;
  get.mutable_get()->set_id("active");
  EXPECT_EQ(call(get).task().state(), research_wire::SUCCEEDED);
  get.mutable_get()->set_id("queued");
  EXPECT_EQ(call(get).task().state(), research_wire::QUEUED);
}

TEST(ResearchTasks, PinnedRiskArtifactSurvivesRestartAndCachedResultsRejectMissingAlgorithm) {
  TaskDirectory directory;
  std::string artifact;
  const auto expected = backtest::run(input());
  const auto file = risk_providers::Module::filename(directory.path / "done");
  {
    tasks::Store store(directory.path);
    artifact = store.submit("done", input()).risk_artifact();
    EXPECT_EQ(artifact, sha256_file(file));
    store.finish("done", store.claim("done"), expected);
    EXPECT_EQ(store.result("done").SerializeAsString(), expected.SerializeAsString());
    const auto saved = directory.path / "original";
    std::filesystem::rename(file, saved);
    EXPECT_THROW(store.result("done"), std::exception);
    std::ofstream(file) << "corrupt fixture";
    EXPECT_THROW(store.result("done"), std::exception);
    EXPECT_EQ(std::filesystem::file_size(file), 15U);
    std::filesystem::remove(file);
    std::filesystem::rename(saved, file);
  }
  tasks::Store recovered(directory.path);
  EXPECT_EQ(recovered.get("done").risk_artifact(), artifact);
  EXPECT_EQ(recovered.result("done").SerializeAsString(), expected.SerializeAsString());
}

#include "bar_dataset_source.hpp"
#include "history_fixture.hpp"
TEST(ResearchDatasets, SettlementDaysWithoutMinuteBarsAreListedAsUncovered) {
  TaskDirectory directory;
  const auto selection =
      test::seed_history(directory.path, {100, 101, 102, 103}, "gap", {"2026-09-23", "2026-09-25"},
                         {"2026-09-23", "2026-09-24", "2026-09-25"});
  tasks::Store store(directory.path);
  Json request = selection;
  request.erase("price_increment");
  request.erase("multiplier");
  request["contract"] = {{"venue", "SHFE"},           {"symbol", "rb2610"},
                         {"product", "rb"},           {"delivery_month", "2026-10"},
                         {"currency", "CNY"},         {"price_increment", "1"},
                         {"quantity_increment", "1"}, {"multiplier", "10"}};
  const auto sources = store.prepare_dataset(protocol::encode_bar_dataset_request(request));
  const auto dataset = tasks::resolve_bar_dataset(sources);
  ASSERT_EQ(dataset.bars_size(), 4);
  EXPECT_EQ(dataset.days_size(), 2);
  ASSERT_EQ(dataset.uncovered_days_size(), 1);
  EXPECT_EQ(dataset.uncovered_days(0), "2026-09-24");
  EXPECT_EQ(dataset.revision(), protocol::bar_dataset_revision(dataset));
}
#include "history_coverage.hpp"
TEST(ResearchDatasets, CoverageCountsTradingDaysPerContractAcrossTheArchive) {
  TaskDirectory directory;
  test::seed_history(directory.path, {100, 101, 102, 103}, "coverage", {"2026-09-23", "2026-09-25"},
                     {"2026-09-23", "2026-09-24", "2026-09-25"});
  {
    tasks::Store store(directory.path);
  } // publishes completed downloads to the archive
  history_files::Archive archive(directory.path / "history");
  data::v1::HistoryFilter filter;
  filter.set_venue("SHFE");
  filter.set_product("rb");
  const auto coverage = tasks::history_coverage(archive, filter);
  ASSERT_EQ(coverage.items_size(), 1);
  const auto& row = coverage.items(0);
  EXPECT_EQ(row.contract_id(), "SHFE/rb/2026-10");
  EXPECT_EQ(row.minute_days(), 2U);
  EXPECT_EQ(row.minute_first(), "2026-09-23");
  EXPECT_EQ(row.daily_days(), 3U);
  EXPECT_EQ(row.uncovered(), 1U);
  ASSERT_EQ(row.uncovered_days_size(), 1);
  EXPECT_EQ(row.uncovered_days(0), "2026-09-24");
  filter.set_product("cu");
  EXPECT_EQ(tasks::history_coverage(archive, filter).items_size(), 0);
}
TEST(Research, PortfolioOfIdenticalContractsDoublesEveryLedgerEffect) {
  const auto single = multiday_input();
  auto pair = single;
  auto* second = pair.mutable_paper()->add_contracts();
  *second = single.paper().contracts(0);
  auto* data = second->mutable_dataset();
  *data->mutable_contract() = test::contract("SHFE", "hc2610", "hc", "2026-10");
  data->set_revision(protocol::bar_dataset_revision(*data));
  pair.set_dataset_revision(protocol::dataset_revision(pair.paper()));
  EXPECT_NE(pair.dataset_revision(), single.dataset_revision());
  const auto one = backtest::run(single);
  const auto two = backtest::run(pair);
  EXPECT_EQ(two.SerializeAsString(), backtest::run(pair).SerializeAsString());
  const auto doubled = [](const protocol::v1::Decimal& value) { return value.units() * 2; };
  EXPECT_EQ(two.account().fees().units(), doubled(one.account().fees()));
  EXPECT_EQ(two.account().realized().units(), doubled(one.account().realized()));
  EXPECT_EQ(two.account().fills_size(), one.account().fills_size() * 2);
  ASSERT_EQ(two.settlements_size(), one.settlements_size());
  for (int day = 0; day < two.settlements_size(); ++day) {
    const auto& settled = two.settlements(day);
    ASSERT_EQ(settled.contracts_size(), 2);
    EXPECT_EQ(settled.contracts(0).symbol(), "rb2610");
    EXPECT_EQ(settled.contracts(1).symbol(), "hc2610");
    EXPECT_EQ(settled.contracts(1).position_quantity().units(),
              one.settlements(day).contracts(0).position_quantity().units());
    // Equity moves twice as far from the shared deposit.
    EXPECT_EQ(settled.equity().units() - d("10000").raw(),
              (one.settlements(day).equity().units() - d("10000").raw()) * 2);
  }
  auto shifted = pair;
  shifted.mutable_paper()->mutable_contracts(1)->mutable_dataset()->mutable_days()->DeleteSubrange(
      1, 1);
  shifted.mutable_paper()->mutable_contracts(1)->mutable_dataset()->mutable_bars()->DeleteSubrange(
      4, 4);
  auto* shifted_data = shifted.mutable_paper()->mutable_contracts(1)->mutable_dataset();
  shifted_data->set_revision(protocol::bar_dataset_revision(*shifted_data));
  shifted.set_dataset_revision(protocol::dataset_revision(shifted.paper()));
  EXPECT_THROW(backtest::run(shifted), std::invalid_argument);
}

TEST(Research, FullResearchBarBudgetRunsThroughFinalSettlement) {
  auto spec = input();
  std::vector<MarketBar> bars;
  for (std::size_t i = 0; i < protocol::max_dataset_bars; ++i)
    bars.push_back(test::flat("2026-09-25", 1790298000000000000LL + i * 1000000LL, "100", "10"));
  *spec.mutable_paper()->mutable_contracts(0)->mutable_dataset() =
      test::dataset(bars, {{"2026-09-25", d("100")}});
  spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
  const auto result = backtest::run(spec);
  EXPECT_EQ(result.equity_size(), protocol::max_dataset_bars + 1);
  ASSERT_EQ(result.settlements_size(), 1);
  EXPECT_EQ(result.settlements(0).balance().units(), d("10000").raw());
  EXPECT_EQ(result.account().cursor(), protocol::max_dataset_bars);
}
