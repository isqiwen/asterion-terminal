#include "node_client.hpp"
#include "support/local_listener.hpp"
#include "support/timing.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/v1/market.pb.h>
#include <asterion/v1/task.pb.h>
#include <asterion/protocol/data_client.hpp>
#include <gtest/gtest.h>
#include <csignal>
#include <fstream>
#include <future>
#include <thread>

using namespace asterion;
using namespace asterion::terminal;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
class NodeSupervision : public testing::Test {
protected:
  ServiceIo io;
  fs::path root;
  std::string endpoint;
  std::unique_ptr<ChildProcess> agent;
  std::shared_ptr<NodeClient> client;
  void SetUp() override {
    root = fs::temp_directory_path() / ("ast-probe-" + unique_process_id().substr(0, 8));
    fs::create_directories(root / "agent");
    endpoint = (root / "agent.sock").string();
    launch();
  }
  void launch() {
    agent = std::make_unique<ChildProcess>(
        ASTERION_AGENT_PATH,
        std::vector<std::string>{"--directory", (root / "agent").string(), "--endpoint", endpoint});
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        client = NodeClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
        return;
      } catch (const std::exception&) {
        ASSERT_FALSE(agent->exited());
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(20ms);
      }
    }
  }
  // The Agent stops, its services with it, and starts again on its directory.
  void restart() {
    client.reset();
    agent.reset();
    launch();
  }
  void TearDown() override {
    client.reset();
    agent.reset();
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  Json health() {
    static_cast<void>(
        client->history_inventory().get()); // An actual Agent status RPC, not its cache.
    return client->status().get().at("health");
  }
  Json state(const std::string& name = "observed") {
    const auto snapshot = health();
    for (const auto& service : snapshot.at("services"))
      if (service.at("id") == name)
        return service;
    throw std::runtime_error("fixture service not found");
  }
  std::unique_ptr<testing_support::LocalListener> intercept(bool dispatch, bool task_health = false,
                                                            const std::string& name = "observed") {
    deploy_fixture(dispatch || task_health, name);
    return attach(dispatch, task_health, name);
  }
  void deploy_fixture(bool task, const std::string& name) {
    const auto kind = task ? node::v1::TASK_SERVICE : node::v1::MARKET_DATA;
    client
        ->deploy({.service = name,
                  .kind = kind,
                  .platform = current_platform(),
                  .programs = local_service_programs(kind),
                  .plugins = std::vector<PluginArtifact>{},
                  .data_service = kind == node::v1::TASK_SERVICE ? name + "-data" : ""})
        .get();
  }
  // Takes over the running fixture service's worker or health socket.
  std::unique_ptr<testing_support::LocalListener> attach(bool dispatch, bool task_health,
                                                         const std::string& name) {
    if (task_health) {
      // The private recovery listener is replaced once initialization finishes.
      // Intercept the steady-state listener, after that handoff has completed.
      const auto ready = std::chrono::steady_clock::now() + 10s;
      while (state(name).at("health") != "ready") {
        if (std::chrono::steady_clock::now() >= ready)
          throw std::runtime_error("fixture task service did not become ready");
        std::this_thread::sleep_for(20ms);
      }
    }
    auto path = fs::path(state(name).at("endpoint").get<std::string>());
    path.replace_extension(dispatch ? ".workers" : ".health");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!fs::exists(path)) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("fixture service did not create its socket");
      std::this_thread::sleep_for(10ms);
    }
    // Replace only the isolated fixture's socket name. An accepted connection
    // stays in this test even when Agent restarts the actual managed process.
    fs::remove(path);
    // Two workers may connect together before this test accepts either claim.
    return std::make_unique<testing_support::LocalListener>(path.string(), dispatch ? 2 : 1);
  }
  Json capacity() { return health().at("worker_capacity"); }
  // Agent records a launch as owned a moment after the worker has started, so
  // the counts are read until they settle.
  void expect_capacity(const Json& expected) {
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(2s);
    auto actual = capacity();
    while (actual != expected && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(20ms);
      actual = capacity();
    }
    EXPECT_EQ(actual, expected);
  }
  void expect_status_while_held() {
    auto read = std::async(std::launch::async, [&] {
      static_cast<void>(client->history_inventory().get());
      return client->status().get();
    });
    EXPECT_EQ(read.wait_for(400ms), std::future_status::ready)
        << "Agent administration waited for a held service RPC";
    EXPECT_EQ(read.get().at("state"), "online");
  }
  void restart_held_process() {
    const auto old = state().at("pid").get<std::uint64_t>();
    // Signal only the PID obtained from this isolated Agent. Instrumented
    // artifact verification can make restart outlast the old RPC's deadline.
    ASSERT_EQ(::kill(static_cast<pid_t>(old), SIGTERM), 0);
    client->action("observed", "restart").get();
    const auto current = state();
    ASSERT_NE(current.at("pid").get<std::uint64_t>(), old);
    ASSERT_EQ(current.at("health"), "starting");
  }
  void complete_stale_probe(testing_support::LocalPeer& held, const std::string& reply,
                            std::chrono::steady_clock::time_point received) {
    try {
      held.send(reply, 1s);
      RecordProperty("stale_reply_sent", "true");
    } catch (const Error& error) {
      // Both completions must be fenced: a late response and the deadline
      // error from a probe whose process was replaced during the RPC. Do not
      // relax production deadlines to accommodate sanitizer startup cost.
      EXPECT_GE(std::chrono::steady_clock::now() - received, 1s);
      EXPECT_EQ(error.code(), ErrorCode::unavailable);
      RecordProperty("stale_reply_sent", "false: probe deadline expired during restart");
    }
  }
};
} // namespace

TEST_F(NodeSupervision,
       SlowHealthLeavesAdministrationAvailableAndOldReplyCannotMarkRestartDegraded) {
  auto listener = intercept(false);
  auto held = listener->accept(8s);
  market::v1::Request request;
  ASSERT_TRUE(request.ParseFromString(held.receive(1s)));
  const auto received = std::chrono::steady_clock::now();
  ASSERT_TRUE(request.has_heartbeat());
  expect_status_while_held();
  restart_held_process();
  market::v1::Response reply;
  reply.set_version(1);
  reply.set_service_id(request.service_id());
  reply.set_correlation_id(request.correlation_id());
  reply.mutable_health()->set_instance_id("retired-process");
  reply.mutable_health()->set_initialized(true);
  reply.mutable_health()->set_phase("error");
  complete_stale_probe(held, reply.SerializeAsString(), received);
  for (int i = 0; i < 8; ++i) {
    const auto current = state();
    EXPECT_NE(current.at("health"), "degraded");
    const auto error = current.at("error").get<std::string>();
    EXPECT_TRUE(error.empty() || (current.at("health") == "starting" &&
                                  error.starts_with("waiting for the first heartbeat: ")))
        << current.dump();
    std::this_thread::sleep_for(20ms);
  }
}

TEST_F(NodeSupervision, SlowTaskDispatchLeavesAdministrationAvailableAndOldLaunchesAreDiscarded) {
  auto listener = intercept(true);
  auto held = listener->accept(5s);
  task::v1::TaskRequest request;
  ASSERT_TRUE(request.ParseFromString(held.receive(1s)));
  const auto received = std::chrono::steady_clock::now();
  ASSERT_TRUE(request.has_dispatch());
  ASSERT_EQ(request.dispatch().launch_slots(), 2);
  expect_capacity(Json{{"limit", 2}, {"owned", 0}, {"reserved", 2}});
  expect_status_while_held();
  restart_held_process();
  task::v1::TaskResponse reply;
  reply.set_version(1);
  reply.set_service_id(request.service_id());
  reply.set_correlation_id(request.correlation_id());
  // A stale reply must be discarded before launch validation or execution.
  reply.mutable_launches()->add_launches()->set_task_id("retired-task");
  complete_stale_probe(held, reply.SerializeAsString(), received);
  for (int i = 0; i < 8; ++i) {
    const auto current = state();
    EXPECT_EQ(current.at("active_workers"), 0);
    const auto error = current.at("error").get<std::string>();
    EXPECT_TRUE(error.empty() || (current.at("health") == "starting" &&
                                  error.starts_with("waiting for the first heartbeat: ")))
        << current.dump();
    std::this_thread::sleep_for(20ms);
  }
}

TEST_F(NodeSupervision, WorkerAllowanceRotatesAcrossServicesAndWaitsForChildrenToExit) {
  // Deploying a service uploads and verifies its programs, which takes long
  // on a loaded machine, and a held worker gives up after its claim timeout.
  // So the second service is deployed before any claim is held and only
  // started while they are.
  deploy_fixture(true, "other");
  client->action("other", "stop").get();
  const auto workers_of_other = [&] {
    auto path = fs::path(state("other").at("endpoint").get<std::string>());
    return path.replace_extension(".workers");
  }();
  for (const auto stopped = std::chrono::steady_clock::now() + testing_support::bound(10s);
       state("other").at("pid") != 0;) {
    ASSERT_LT(std::chrono::steady_clock::now(), stopped) << "fixture service did not stop";
    std::this_thread::sleep_for(20ms);
  }
  // attach() must meet the socket of the instance started below.
  fs::remove(workers_of_other);
  auto first = intercept(true);
  auto dispatch = first->accept(5s);
  task::v1::TaskRequest request;
  ASSERT_TRUE(request.ParseFromString(dispatch.receive(1s)));
  ASSERT_TRUE(request.has_dispatch());
  ASSERT_EQ(request.dispatch().launch_slots(), 2);
  task::v1::TaskResponse response;
  response.set_version(1);
  response.set_service_id(request.service_id());
  response.set_correlation_id(request.correlation_id());
  for (const auto* id : {"held-one", "held-two"}) {
    auto* launch = response.mutable_launches()->add_launches();
    launch->set_task_id(id);
    launch->set_program(task::v1::FACTOR_PROGRAM);
  }
  dispatch.send(response.SerializeAsString(), 1s);
  // Real worker processes are held before claim; no fabricated market input runs.
  std::vector<testing_support::LocalPeer> workers;
  for (int i = 0; i < 2; ++i) {
    auto worker = first->accept(5s);
    task::v1::TaskRequest claim;
    ASSERT_TRUE(claim.ParseFromString(worker.receive(1s)));
    ASSERT_TRUE(claim.has_claim()) << claim.DebugString() << state().dump();
    workers.push_back(std::move(worker));
  }
  client->action("other", "start").get();
  auto second = attach(true, false, "other");
  expect_capacity(Json{{"limit", 2}, {"owned", 2}, {"reserved", 0}});
  EXPECT_EQ(state("other").at("active_workers"), 0);
  // More than one supervision cycle passes; a second service receives no offer.
  EXPECT_THROW(second->accept(1200ms), std::exception);
  EXPECT_EQ(capacity().at("owned"), 2);
  // Both services remain ready and the first is eligible for another turn.
  // End one worker at the held claim boundary. The later service must get its
  // freed slot before the earlier name in Agent's observation map can refill.
  workers.pop_back();
  auto next = second->accept(5s);
  task::v1::TaskRequest offered;
  ASSERT_TRUE(offered.ParseFromString(next.receive(1s)));
  ASSERT_TRUE(offered.has_dispatch());
  ASSERT_EQ(offered.dispatch().launch_slots(), 1);
  expect_capacity(Json{{"limit", 2}, {"owned", 1}, {"reserved", 1}});
  EXPECT_THROW(first->accept(200ms), std::exception);
  response.set_service_id(offered.service_id());
  response.set_correlation_id(offered.correlation_id());
  response.mutable_launches()->clear_launches();
  auto* launch = response.mutable_launches()->add_launches();
  launch->set_task_id("held-three");
  launch->set_program(task::v1::FACTOR_PROGRAM);
  next.send(response.SerializeAsString(), 1s);
  {
    auto worker = second->accept(5s);
    task::v1::TaskRequest claim;
    ASSERT_TRUE(claim.ParseFromString(worker.receive(1s)));
    ASSERT_TRUE(claim.has_claim()) << claim.DebugString() << state("other").dump();
    workers.push_back(std::move(worker));
  }
  expect_capacity(Json{{"limit", 2}, {"owned", 2}, {"reserved", 0}});
  client->action("other", "stop").get();
  auto returned = first->accept(5s);
  ASSERT_TRUE(offered.ParseFromString(returned.receive(1s)));
  ASSERT_TRUE(offered.has_dispatch());
  EXPECT_EQ(offered.dispatch().launch_slots(), 1);
  expect_capacity(Json{{"limit", 2}, {"owned", 1}, {"reserved", 1}});
  response.set_service_id(offered.service_id());
  response.set_correlation_id(offered.correlation_id());
  response.mutable_launches()->clear_launches();
  returned.send(response.SerializeAsString(), 1s);
  client->action("observed", "stop").get();
  expect_capacity(Json{{"limit", 2}, {"owned", 0}, {"reserved", 0}});
}

TEST_F(NodeSupervision, InitializingTaskServiceKeepsHeartbeatAndBecomesReadyWithoutRestart) {
  auto listener = intercept(false, true);
  const auto original = state().at("pid");
  const auto begin = std::chrono::steady_clock::now();
  const auto deadline = begin + 20s;
  bool ready_sent = false;
  unsigned initializing = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    auto held = listener->accept(6s);
    task::v1::TaskRequest request;
    ASSERT_TRUE(request.ParseFromString(held.receive(1s)));
    ASSERT_TRUE(request.has_heartbeat());
    task::v1::TaskResponse reply;
    reply.set_version(1);
    reply.set_service_id(request.service_id());
    reply.set_correlation_id(request.correlation_id());
    reply.mutable_health()->set_instance_id(request.service_id());
    reply.mutable_health()->set_data_instance("observed-data");
    ready_sent = std::chrono::steady_clock::now() - begin >= 11s;
    reply.mutable_health()->set_initialized(ready_sent);
    held.send(reply.SerializeAsString(), 1s);
    auto observed = state();
    const auto applied = std::chrono::steady_clock::now() + 1s;
    while (observed.at("health") != (ready_sent ? "ready" : "starting") &&
           std::chrono::steady_clock::now() < applied) {
      std::this_thread::sleep_for(10ms);
      observed = state();
    }
    EXPECT_EQ(observed.at("pid"), original);
    EXPECT_NE(observed.at("health"), "unresponsive");
    if (!ready_sent) {
      EXPECT_EQ(observed.at("health"), "starting");
      EXPECT_EQ(observed.at("active_workers"), 0);
      ++initializing;
    } else if (observed.at("health") == "ready")
      break;
  }
  EXPECT_GT(initializing, 1U);
  EXPECT_TRUE(ready_sent);
  EXPECT_EQ(state().at("health"), "ready");
  EXPECT_EQ(state().at("pid"), original);
}

TEST_F(NodeSupervision, ServiceWaitsForResourcesWithoutSpendingRestartsAndStartsAfterRelease) {
  const auto budget = health().at("resource_budget");
  const auto limit = budget.at("limit");
  // Market services ask for themselves alone, so the node holds a whole number of them.
  const auto name = [](unsigned index) { return "market-" + std::to_string(index); };
  deploy_fixture(false, name(0));
  const auto first = state(name(0));
  ASSERT_EQ(first.at("state"), "running");
  const auto request = first.at("resource_request");
  const auto count =
      std::min({limit.at("cpu_slots").get<unsigned>() / request.at("cpu_slots").get<unsigned>(),
                limit.at("memory_mib").get<unsigned>() / request.at("memory_mib").get<unsigned>(),
                limit.at("io_slots").get<unsigned>() / request.at("io_slots").get<unsigned>()});
  for (unsigned i = 1; i <= count; ++i)
    deploy_fixture(false, name(i));
  const auto queued = name(count);
  const auto address = state(queued).at("endpoint").get<std::string>();
  ASSERT_FALSE(address.empty());
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    const auto waiting = state(queued);
    EXPECT_EQ(waiting.at("state"), "waiting_capacity");
    EXPECT_EQ(waiting.at("pid"), 0);
    EXPECT_EQ(waiting.at("restarts"), 0);
    const auto used = health().at("resource_budget").at("committed");
    for (const auto* field : {"cpu_slots", "memory_mib", "io_slots"})
      EXPECT_EQ(used.at(field), count * request.at(field).get<unsigned>());
    std::this_thread::sleep_for(50ms);
  } while (std::chrono::steady_clock::now() < deadline);
  client->action(name(0), "stop").get();
  const auto resumed = std::chrono::steady_clock::now() + 8s;
  while (state(queued).at("state") != "running" && std::chrono::steady_clock::now() < resumed)
    std::this_thread::sleep_for(20ms);
  EXPECT_EQ(state(queued).at("state"), "running");
  EXPECT_EQ(state(queued).at("restarts"), 0);
  EXPECT_EQ(state(queued).at("endpoint"), address);
}

// A task service can only queue without a worker, so one worker's worth of the
// node is kept with it: other services wait rather than take it.
TEST_F(NodeSupervision, OtherServicesLeaveATaskServiceTheWorkerItNeeds) {
  deploy_fixture(true, "observed");
  const auto task = state().at("resource_request");
  unsigned markets = 0;
  std::string queued;
  for (;; ++markets) {
    ASSERT_LT(markets, 32U) << "the node admitted every service";
    queued = "market-" + std::to_string(markets);
    deploy_fixture(false, queued);
    if (state(queued).at("state") == "waiting_capacity")
      break;
  }
  ASSERT_GT(markets, 0U);
  // What is committed counts that worker though none runs.
  const auto market = state("market-0").at("resource_request");
  const auto committed = health().at("resource_budget").at("committed");
  const Json worker{{"cpu_slots", 1}, {"memory_mib", 1024}, {"io_slots", 1}};
  for (const auto* field : {"cpu_slots", "memory_mib", "io_slots"})
    EXPECT_EQ(committed.at(field).get<unsigned>(), task.at(field).get<unsigned>() +
                                                       markets * market.at(field).get<unsigned>() +
                                                       worker.at(field).get<unsigned>())
        << field;
  EXPECT_EQ(capacity().at("owned"), 0);
  // And the Agent goes on offering it to the task service.
  auto listener = attach(true, false, "observed");
  auto offer = listener->accept(5s);
  task::v1::TaskRequest request;
  ASSERT_TRUE(request.ParseFromString(offer.receive(1s)));
  ASSERT_TRUE(request.has_dispatch());
  EXPECT_GE(request.dispatch().launch_slots(), 1U);
}

// A data service is deployed for a task service. While the pair cannot run
// beside what the node holds, it waits whole instead of taking room from the
// pair at work, and starts once that room is given up.
TEST_F(NodeSupervision, ADataServiceWaitsUntilItsPairHasRoom) {
  const auto data = [&](const std::string& name, const std::string& task) {
    client
        ->deploy({.service = name,
                  .kind = node::v1::DATA_SERVICE,
                  .platform = current_platform(),
                  .programs = local_service_programs(node::v1::DATA_SERVICE),
                  .plugins = std::vector<PluginArtifact>{},
                  .task_service = task})
        .get();
  };
  data("observed-data", "observed");
  deploy_fixture(true, "observed");
  ASSERT_EQ(state("observed-data").at("state"), "running");
  ASSERT_EQ(state("observed").at("state"), "running");
  const auto working = health().at("resource_budget").at("committed");
  // A second pair is on its way: its data service alone would fit.
  data("spare-data", "spare");
  const auto waiting = state("spare-data");
  EXPECT_EQ(waiting.at("state"), "waiting_capacity");
  EXPECT_EQ(waiting.at("pid"), 0);
  EXPECT_EQ(waiting.at("error"),
            "Node resource capacity cannot hold this service together with the other half of its "
            "data and task pair and one worker; service is waiting");
  EXPECT_EQ(health().at("resource_budget").at("committed"), working);
  // The pair at work is offered what it was offered before.
  {
    auto listener = attach(true, false, "observed");
    auto offer = listener->accept(5s);
    task::v1::TaskRequest request;
    ASSERT_TRUE(request.ParseFromString(offer.receive(1s)));
    ASSERT_TRUE(request.has_dispatch());
    EXPECT_EQ(request.dispatch().launch_slots(), capacity().at("limit"));
  }
  // The owner gives up the first pair: the second has room, half by half.
  client->action("observed", "stop").get();
  client->action("observed-data", "stop").get();
  const auto started = std::chrono::steady_clock::now() + 8s;
  while (state("spare-data").at("state") != "running" && std::chrono::steady_clock::now() < started)
    std::this_thread::sleep_for(20ms);
  EXPECT_EQ(state("spare-data").at("state"), "running");
  deploy_fixture(true, "spare");
  EXPECT_EQ(state("spare").at("state"), "running");
  EXPECT_EQ(health().at("resource_budget").at("committed"), working);
}

// Room for one half is not room for the pair: neither starts until both fit.
TEST_F(NodeSupervision, NeitherHalfOfAPairStartsWithoutRoomForBoth) {
  // Four market services leave room for a task service and its worker, or for
  // a data service, but not for both.
  for (int index = 0; index < 4; ++index)
    deploy_fixture(false, "market-" + std::to_string(index));
  client
      ->deploy({.service = "observed-data",
                .kind = node::v1::DATA_SERVICE,
                .platform = current_platform(),
                .programs = local_service_programs(node::v1::DATA_SERVICE),
                .plugins = std::vector<PluginArtifact>{},
                .task_service = "observed"})
      .get();
  deploy_fixture(true, "observed");
  for (const auto* half : {"observed-data", "observed"}) {
    EXPECT_EQ(state(half).at("state"), "waiting_capacity") << half;
    EXPECT_EQ(state(half).at("pid"), 0) << half;
  }
  client->action("market-0", "stop").get();
  const auto started = std::chrono::steady_clock::now() + 8s;
  const auto whole = [&] {
    return state("observed-data").at("state") == "running" &&
           state("observed").at("state") == "running";
  };
  while (!whole() && std::chrono::steady_clock::now() < started)
    std::this_thread::sleep_for(20ms);
  EXPECT_TRUE(whole());
  // Nothing is left over: three markets, the pair and its worker fill the node.
  const auto budget = health().at("resource_budget");
  EXPECT_EQ(budget.at("committed").at("cpu_slots"), budget.at("limit").at("cpu_slots"));
}

// When not everything fits, who runs is not reshuffled by a start of the Agent
// or by a crash: a service that held its reservation takes it again before
// one that was waiting is tried, whatever their names.
TEST_F(NodeSupervision, AServiceThatHeldItsReservationTakesItAgainBeforeOneThatWaited) {
  const auto limit = health().at("resource_budget").at("limit");
  const auto held = [](unsigned index) { return "held-" + std::to_string(index); };
  deploy_fixture(false, held(0));
  const auto request = state(held(0)).at("resource_request");
  const auto count =
      std::min({limit.at("cpu_slots").get<unsigned>() / request.at("cpu_slots").get<unsigned>(),
                limit.at("memory_mib").get<unsigned>() / request.at("memory_mib").get<unsigned>(),
                limit.at("io_slots").get<unsigned>() / request.at("io_slots").get<unsigned>()});
  for (unsigned index = 1; index < count; ++index)
    deploy_fixture(false, held(index));
  // The node is full. One more waits; supervision visits it first, by its name.
  deploy_fixture(false, "a-waiting");
  ASSERT_EQ(state("a-waiting").at("state"), "waiting_capacity");
  const auto running = [&] {
    unsigned result = 0;
    for (unsigned index = 0; index < count; ++index)
      result += state(held(index)).at("state") == "running" ? 1U : 0U;
    return result;
  };
  ASSERT_EQ(running(), count);
  // The Agent writes down who holds a reservation; then it is stopped.
  const auto record = root / "agent" / "reservations.json";
  const auto recorded = [&] {
    std::ifstream file(record);
    const auto text = std::string(std::istreambuf_iterator<char>(file), {});
    return fs::exists(record) && !text.empty() && parse_json(text).at("services").size() == count;
  };
  const auto written = std::chrono::steady_clock::now() + 5s;
  while (!recorded() && std::chrono::steady_clock::now() < written)
    std::this_thread::sleep_for(20ms);
  ASSERT_TRUE(recorded());
  restart();
  // The waiting service is left be until the others have had their turn, and
  // then finds the node as full as it was.
  const auto back = std::chrono::steady_clock::now() + 15s;
  while ((running() != count || state("a-waiting").at("state") != "waiting_capacity") &&
         std::chrono::steady_clock::now() < back)
    std::this_thread::sleep_for(50ms);
  EXPECT_EQ(running(), count);
  EXPECT_EQ(state("a-waiting").at("state"), "waiting_capacity");
  EXPECT_EQ(state("a-waiting").at("pid"), 0);
  // One of them crashes moments after it started, so its restart has to wait
  // for its turn while the waiting service asks again every second.
  const auto crashed = state(held(0)).at("pid").get<std::uint64_t>();
  ASSERT_EQ(::kill(static_cast<pid_t>(crashed), SIGKILL), 0);
  const auto restarted = std::chrono::steady_clock::now() + 15s;
  const auto returned = [&] {
    const auto service = state(held(0));
    return service.at("state") == "running" && service.at("pid") != crashed;
  };
  while (!returned() && std::chrono::steady_clock::now() < restarted)
    std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(returned());
  EXPECT_EQ(state("a-waiting").at("state"), "waiting_capacity");
}

TEST_F(NodeSupervision, DataInstanceRunsIndependentlyAndKeepsItsBindingOnRestart) {
  client
      ->deploy({.service = "data-one",
                .kind = node::v1::DATA_SERVICE,
                .platform = current_platform(),
                .programs = local_service_programs(node::v1::DATA_SERVICE),
                .plugins = std::vector<PluginArtifact>{},
                .task_service = "tasks-one"})
      .get();
  auto ready = [&] {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      const auto service = state("data-one");
      if (service.at("health") == "ready")
        return service;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(service.dump());
      std::this_thread::sleep_for(20ms);
    }
  };
  auto first = ready();
  EXPECT_EQ(first.at("kind"), "data");
  EXPECT_EQ(first.at("task_service"), "tasks-one");
  protocol::DataClient data(first.at("endpoint").get<std::string>(), "data-one");
  data::v1::DataRequest heartbeat;
  heartbeat.mutable_heartbeat();
  const auto before = data.call(heartbeat).health();
  EXPECT_EQ(before.instance_id(), "data-one");
  client->action("data-one", "restart").get();
  (void)ready();
  const auto after = data.call(heartbeat).health();
  EXPECT_EQ(after.instance_id(), before.instance_id());
  EXPECT_NE(after.process_id(), before.process_id());
  EXPECT_EQ(state("data-one").at("task_service"), "tasks-one");
}

TEST_F(NodeSupervision, TaskBindingSurvivesRestartWithoutADataProcess) {
  client
      ->deploy({.service = "tasks-one",
                .kind = node::v1::TASK_SERVICE,
                .platform = current_platform(),
                .programs = local_service_programs(node::v1::TASK_SERVICE),
                .plugins = std::vector<PluginArtifact>{},
                .data_service = "data-one"})
      .get();
  auto ready = [&] {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      const auto service = state("tasks-one");
      if (service.at("health") == "ready")
        return service;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(service.dump());
      std::this_thread::sleep_for(20ms);
    }
  };
  const auto endpoint = ready().at("endpoint").get<std::string>();
  auto call = [&](task::v1::TaskRequest request) {
    request.set_version(1);
    request.set_service_id("tasks-one");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(endpoint, 2s);
    channel.send(request.SerializeAsString(), 2s);
    task::v1::TaskResponse reply;
    if (!reply.ParseFromString(channel.receive(2s)) || reply.has_error())
      throw std::runtime_error("task fixture request failed");
    return reply;
  };
  task::v1::TaskRequest heartbeat;
  heartbeat.mutable_heartbeat();
  const auto before = call(heartbeat).health();
  EXPECT_EQ(before.instance_id(), "tasks-one");
  EXPECT_EQ(before.data_instance(), "data-one");
  task::v1::TaskRequest list;
  list.mutable_list()->set_limit(200);
  EXPECT_EQ(call(list).tasks().tasks_size(), 0);
  client->action("tasks-one", "restart").get();
  EXPECT_EQ(ready().at("data_service"), "data-one");
  const auto after = call(heartbeat).health();
  EXPECT_EQ(after.instance_id(), before.instance_id());
  EXPECT_NE(after.process_id(), before.process_id());
  EXPECT_EQ(after.data_instance(), before.data_instance());
  EXPECT_THROW(client
                   ->deploy({.service = "data-one",
                             .kind = node::v1::DATA_SERVICE,
                             .platform = current_platform(),
                             .programs = local_service_programs(node::v1::DATA_SERVICE),
                             .plugins = std::vector<PluginArtifact>{},
                             .task_service = "another-task"})
                   .get(),
               std::exception);
  client
      ->deploy({.service = "data-one",
                .kind = node::v1::DATA_SERVICE,
                .platform = current_platform(),
                .programs = local_service_programs(node::v1::DATA_SERVICE),
                .plugins = std::vector<PluginArtifact>{},
                .task_service = "tasks-one"})
      .get();
  const auto pair = client->data_task_endpoints("tasks-one").get();
  EXPECT_EQ(pair.task.endpoint, endpoint);
  EXPECT_EQ(pair.data.session, "data-one");
  EXPECT_FALSE(pair.data.endpoint.empty());
  client->action("data-one", "stop").get();
  client->action("tasks-one", "stop").get();
  const auto stopped = client->data_task_endpoints("tasks-one").get();
  EXPECT_TRUE(same_service_endpoint(pair.task, stopped.task));
  EXPECT_TRUE(same_service_endpoint(pair.data, stopped.data));
}

TEST_F(NodeSupervision, SlowProcessReleaseKeepsStatusAndConfirmedStopIntentVisible) {
  client
      ->deploy({.service = "observed",
                .kind = node::v1::MARKET_DATA,
                .platform = current_platform(),
                .programs = local_service_programs(node::v1::MARKET_DATA),
                .plugins = std::vector<PluginArtifact>{}})
      .get();
  // Inject the fault into an initialized service, after it has installed its
  // shutdown handler and Agent has received its first health observation.
  const auto ready = std::chrono::steady_clock::now() + 10s;
  while (state().at("health") == "starting") {
    ASSERT_LT(std::chrono::steady_clock::now(), ready);
    std::this_thread::sleep_for(10ms);
  }
  const auto pid = state().at("pid").get<std::uint64_t>();
  ASSERT_NE(pid, 0U);
  // A stopped child cannot handle SIGTERM: release must wait and then kill it.
  ASSERT_EQ(::kill(static_cast<pid_t>(pid), SIGSTOP), 0);
  auto stopping = std::async(std::launch::async, [&] {
    auto control = NodeClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
    control->action("observed", "stop").get();
  });
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  Json observed;
  do {
    const auto begin = std::chrono::steady_clock::now();
    observed = state();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 400ms);
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    if (observed.at("state") == "stopping")
      break;
    std::this_thread::sleep_for(10ms);
  } while (stopping.wait_for(0ms) != std::future_status::ready);
  EXPECT_EQ(observed.at("state"), "stopping");
  EXPECT_FALSE(observed.at("desired_running").get<bool>());
  EXPECT_EQ(observed.at("pid"), pid);
  EXPECT_EQ(health().at("resource_budget").at("committed"), observed.at("resource_request"));
  EXPECT_EQ(stopping.wait_for(0ms), std::future_status::timeout);
  EXPECT_NO_THROW(stopping.get());
  EXPECT_EQ(state().at("state"), "stopped");
  EXPECT_EQ(state().at("pid"), 0);
  EXPECT_EQ(health().at("resource_budget").at("committed"),
            (Json{{"cpu_slots", 0}, {"memory_mib", 0}, {"io_slots", 0}}));
}

TEST_F(NodeSupervision, FailedCommitFreezesMutationsAndPreservesConfirmedConfiguration) {
  client
      ->deploy({.service = "observed",
                .kind = node::v1::MARKET_DATA,
                .platform = current_platform(),
                .programs = local_service_programs(node::v1::MARKET_DATA),
                .plugins = std::vector<PluginArtifact>{}})
      .get();
  const auto before = state();
  const auto config = root / "agent/services/observed/service.json";
  const auto pending = root / "agent/services/observed/service.pending";
  const auto digest = sha256_file(config);
  write_file_durably(pending, "test-owned unfinished publication");
  EXPECT_THROW(client->action("observed", "stop").get(), Error);
  auto observer = NodeClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
  const auto health = observer->status().get().at("health");
  EXPECT_EQ(health.at("phase"), "recovery_required");
  EXPECT_EQ(health.at("failure").at("message"),
            "Agent configuration commit failed; inspect node logs: unfinished service "
            "configuration requires explicit recovery");
  EXPECT_FALSE(health.at("execution").at("business_ready").get<bool>());
  EXPECT_TRUE(health.at("execution").at("persistence").at("observed").get<bool>());
  EXPECT_EQ(health.at("services").at(0).at("revision"), before.at("revision"));
  EXPECT_TRUE(health.at("services").at(0).at("desired_running").get<bool>());
  EXPECT_EQ(health.at("services").at(0).at("pid"), before.at("pid"));
  EXPECT_THROW(client->action("observed", "restart").get(), Error);
  EXPECT_THROW((void)client->history_inventory().get(), Error);
  EXPECT_EQ(sha256_file(config), digest);
  EXPECT_TRUE(fs::exists(pending));
  agent->request_stop();
  ASSERT_TRUE(agent->wait(6s));
  EXPECT_EQ(agent->exit_code(), 0);
  EXPECT_FALSE(process_running(before.at("pid").get<std::uint64_t>()));
  EXPECT_EQ(sha256_file(config), digest);
  EXPECT_TRUE(fs::exists(pending));
}

TEST_F(NodeSupervision, IncompleteStartupRemainsObservableWithoutPublishingAPartialInventory) {
  client
      ->deploy({.service = "observed",
                .kind = node::v1::MARKET_DATA,
                .platform = current_platform(),
                .programs = local_service_programs(node::v1::MARKET_DATA),
                .plugins = std::vector<PluginArtifact>{}})
      .get();
  client.reset();
  agent.reset();
  const auto config = root / "agent/services/observed/service.json";
  const auto digest = sha256_file(config);
  // A separate incomplete service must prevent publishing the otherwise valid inventory.
  fs::create_directories(root / "agent/services/incomplete");
  const auto pending = root / "agent/services/incomplete/service.pending";
  write_file_durably(pending, "test-owned unfinished publication");
  agent = std::make_unique<ChildProcess>(
      ASTERION_AGENT_PATH,
      std::vector<std::string>{"--directory", (root / "agent").string(), "--endpoint", endpoint});
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  Json health;
  for (;;) {
    try {
      client = NodeClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
    } catch (const Error&) {
      ASSERT_FALSE(agent->exited());
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      std::this_thread::sleep_for(20ms);
      continue;
    }
    health = client->status().get().at("health");
    ASSERT_TRUE(health.at("services").empty());
    if (health.at("phase") == "recovery_required")
      break;
    EXPECT_EQ(health.at("phase"), "initializing");
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_EQ(health.at("failure").at("message"),
            "Agent initialization failed; inspect node logs: unfinished service configuration "
            "requires explicit recovery");
  EXPECT_TRUE(health.at("execution").at("io").at("observed").get<bool>());
  EXPECT_TRUE(health.at("execution").at("initialization").at("observed").get<bool>());
  EXPECT_FALSE(health.at("execution").at("initialization").at("pending").get<bool>());
  EXPECT_THROW(client->action("observed", "start").get(), Error);
  EXPECT_EQ(sha256_file(config), digest);
  EXPECT_TRUE(fs::exists(pending));
  EXPECT_FALSE(agent->exited());
  agent->request_stop();
  ASSERT_TRUE(agent->wait(6s));
  EXPECT_EQ(agent->exit_code(), 0);
  EXPECT_EQ(sha256_file(config), digest);
  EXPECT_TRUE(fs::exists(pending));
}
