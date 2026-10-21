#include "agent_client.hpp"
#include "node_paths.hpp"
#include "support/local_listener.hpp"
#include "support/timing.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/market.pb.h>
#include <asterion/task.pb.h>
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

class NodeSupervision : public testing::Test
{
protected:
    ServiceIo io;
    fs::path root;
    std::string endpoint;
    std::unique_ptr<ChildProcess> agent;
    std::shared_ptr<AgentClient> client;

    void SetUp() override
    {
        root = fs::temp_directory_path() / ("ast-probe-" + unique_process_id().substr(0, 8));
        fs::create_directories(root / "agent");
        install_node_configuration(
            root / "agent",
            fs::path(ASTERION_AGENT_PATH).parent_path() / "config");
        endpoint = (root / "agent.sock").string();
        launch();
    }

    void launch()
    {
        agent = std::make_unique<ChildProcess>(
            ASTERION_AGENT_PATH,
            std::vector<std::string>{
                "--directory",
                (root / "agent").string(),
                "--endpoint",
                endpoint
            });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                client = AgentClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint})
                             .get();
                return;
            } catch (const std::exception&) {
                ASSERT_FALSE(agent->exited());
                ASSERT_LT(std::chrono::steady_clock::now(), deadline);
                std::this_thread::sleep_for(20ms);
            }
        }
    }

    // The Agent stops, its services with it, and starts again on its directory.
    void restart()
    {
        client.reset();
        agent.reset();
        launch();
    }

    void TearDown() override
    {
        client.reset();
        agent.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    Json health()
    {
        static_cast<void>(
            client->history_inventory().get()); // An actual Agent status RPC, not its cache.
        return client->status().get().at("health");
    }

    Json state(const std::string& name = "observed")
    {
        const auto snapshot = health();
        for (const auto& service : snapshot.at("services"))
            if (service.at("id") == name)
                return service;
        throw std::runtime_error("fixture service not found");
    }

    std::unique_ptr<testing_support::LocalListener>
    intercept(bool dispatch, bool task_health = false, const std::string& name = "observed")
    {
        deploy_fixture(dispatch || task_health, name);
        return attach(dispatch, task_health, name);
    }

    void deploy_fixture(bool task, const std::string& name)
    {
        const auto kind = task ? node::proto::TASK_SERVICE : node::proto::MARKET_DATA;
        client
            ->deploy(
                {.service = name,
                    .kind = kind,
                    .platform = current_platform(),
                    .programs = local_service_programs(kind),
                    .plugins = std::vector<PluginArtifact>{},
                    .data_service = kind == node::proto::TASK_SERVICE ? name + "-data" : ""})
            .get();
    }

    // Takes over the running fixture service's worker or health socket.
    std::unique_ptr<testing_support::LocalListener>
    attach(bool dispatch, bool task_health, const std::string& name)
    {
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

    void expect_status_while_held()
    {
        auto read = std::async(std::launch::async, [&] {
            static_cast<void>(client->history_inventory().get());
            return client->status().get();
        });
        EXPECT_EQ(read.wait_for(400ms), std::future_status::ready)
            << "Agent administration waited for a held service RPC";
        EXPECT_EQ(read.get().at("state"), "online");
    }

    void restart_held_process()
    {
        const auto old = state().at("pid").get<std::uint64_t>();
        // Signal only the PID obtained from this isolated Agent. Instrumented
        // artifact verification can make restart outlast the old RPC's deadline.
        ASSERT_EQ(::kill(static_cast<pid_t>(old), SIGTERM), 0);
        client->action("observed", "restart").get();
        const auto current = state();
        ASSERT_NE(current.at("pid").get<std::uint64_t>(), old);
        ASSERT_EQ(current.at("health"), "starting");
    }

    void complete_stale_probe(
        testing_support::LocalPeer& held,
        const std::string& reply,
        std::chrono::steady_clock::time_point received)
    {
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

TEST_F(NodeSupervision, ResourceObservationFailureDoesNotChangeReadiness)
{
    const auto directory = root / "agent";
    const auto detached = root / "detached";
    fs::rename(directory, detached);

    struct Restore
    {
        fs::path from, to;

        ~Restore()
        {
            std::error_code error;
            fs::rename(from, to, error);
        }
    } restore{detached, directory};

    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(10s);
    Json observed;
    do {
        observed = health();
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(50ms);
    } while (observed.at("host_resources").is_null()
        || observed.at("host_resources").at("failure").is_null());
    EXPECT_EQ(observed.at("phase"), "ready");
    EXPECT_EQ(observed.at("host_resources").at("failure").at("code"), "unavailable");
    EXPECT_FALSE(agent->exited());
}

TEST_F(NodeSupervision, SlowHealthLeavesAdministrationAvailableAndOldReplyCannotMarkRestartDegraded)
{
    auto listener = intercept(false);
    auto held = listener->accept(8s);
    market::proto::Request request;
    ASSERT_TRUE(request.ParseFromString(held.receive(1s)));
    const auto received = std::chrono::steady_clock::now();
    ASSERT_TRUE(request.has_heartbeat());
    expect_status_while_held();
    restart_held_process();
    market::proto::Response reply;
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
        EXPECT_TRUE(
            error.empty()
            || (current.at("health") == "starting"
                && error.starts_with("waiting for the first heartbeat: ")))
            << current.dump();
        std::this_thread::sleep_for(20ms);
    }
}

TEST_F(NodeSupervision, InitializingTaskServiceKeepsHeartbeatAndBecomesReadyWithoutRestart)
{
    auto listener = intercept(false, true);
    const auto original = state().at("pid");
    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + 20s;
    bool ready_sent = false;
    unsigned initializing = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        auto held = listener->accept(6s);
        task::proto::TaskRequest request;
        ASSERT_TRUE(request.ParseFromString(held.receive(1s)));
        ASSERT_TRUE(request.has_heartbeat());
        task::proto::TaskResponse reply;
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
        while (observed.at("health") != (ready_sent ? "ready" : "starting")
            && std::chrono::steady_clock::now() < applied) {
            std::this_thread::sleep_for(10ms);
            observed = state();
        }
        EXPECT_EQ(observed.at("pid"), original);
        EXPECT_NE(observed.at("health"), "unresponsive");
        if (!ready_sent) {
            EXPECT_EQ(observed.at("health"), "starting");
            ++initializing;
        } else if (observed.at("health") == "ready")
            break;
    }
    EXPECT_GT(initializing, 1U);
    EXPECT_TRUE(ready_sent);
    EXPECT_EQ(state().at("health"), "ready");
    EXPECT_EQ(state().at("pid"), original);
}

TEST_F(NodeSupervision, DataInstanceRunsIndependentlyAndKeepsItsBindingOnRestart)
{
    client
        ->deploy(
            {.service = "data-one",
                .kind = node::proto::DATA_SERVICE,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::DATA_SERVICE),
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
    data::proto::DataRequest heartbeat;
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

TEST_F(NodeSupervision, TaskBindingSurvivesRestartWithoutADataProcess)
{
    client
        ->deploy(
            {.service = "tasks-one",
                .kind = node::proto::TASK_SERVICE,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::TASK_SERVICE),
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
    auto call = [&](task::proto::TaskRequest request) {
        request.set_version(1);
        request.set_service_id("tasks-one");
        request.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(endpoint, 2s);
        channel.send(request.SerializeAsString(), 2s);
        task::proto::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(2s)) || reply.has_error())
            throw std::runtime_error("task fixture request failed");
        return reply;
    };
    task::proto::TaskRequest heartbeat;
    heartbeat.mutable_heartbeat();
    const auto before = call(heartbeat).health();
    EXPECT_EQ(before.instance_id(), "tasks-one");
    EXPECT_EQ(before.data_instance(), "data-one");
    task::proto::TaskRequest list;
    list.mutable_list()->set_limit(200);
    EXPECT_EQ(call(list).tasks().tasks_size(), 0);
    client->action("tasks-one", "restart").get();
    EXPECT_EQ(ready().at("data_service"), "data-one");
    const auto after = call(heartbeat).health();
    EXPECT_EQ(after.instance_id(), before.instance_id());
    EXPECT_NE(after.process_id(), before.process_id());
    EXPECT_EQ(after.data_instance(), before.data_instance());
    EXPECT_THROW(
        client
            ->deploy(
                {.service = "data-one",
                    .kind = node::proto::DATA_SERVICE,
                    .platform = current_platform(),
                    .programs = local_service_programs(node::proto::DATA_SERVICE),
                    .plugins = std::vector<PluginArtifact>{},
                    .task_service = "another-task"})
            .get(),
        std::exception);
    client
        ->deploy(
            {.service = "data-one",
                .kind = node::proto::DATA_SERVICE,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::DATA_SERVICE),
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

TEST_F(NodeSupervision, SlowProcessReleaseKeepsStatusAndConfirmedStopIntentVisible)
{
    client
        ->deploy(
            {.service = "observed",
                .kind = node::proto::MARKET_DATA,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::MARKET_DATA),
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
        auto control =
            AgentClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
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
    EXPECT_EQ(stopping.wait_for(0ms), std::future_status::timeout);
    EXPECT_NO_THROW(stopping.get());
    EXPECT_EQ(state().at("state"), "stopped");
    EXPECT_EQ(state().at("pid"), 0);
}

TEST_F(NodeSupervision, FailedCommitFreezesMutationsAndPreservesConfirmedConfiguration)
{
    client
        ->deploy(
            {.service = "observed",
                .kind = node::proto::MARKET_DATA,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::MARKET_DATA),
                .plugins = std::vector<PluginArtifact>{}})
        .get();
    const auto before = state();
    const auto config = root / "agent/services/observed/service.json";
    const auto pending = root / "agent/services/observed/service.pending";
    const auto digest = sha256_file(config);
    write_file_durably(pending, "test-owned unfinished publication");
    EXPECT_THROW(client->action("observed", "stop").get(), Error);
    auto observer =
        AgentClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
    const auto health = observer->status().get().at("health");
    EXPECT_EQ(health.at("phase"), "recovery_required");
    EXPECT_EQ(
        health.at("failure").at("message"),
        "Agent configuration commit failed; inspect node logs: unfinished service " "configuration requires explicit recovery");
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

TEST_F(NodeSupervision, IncompleteStartupRemainsObservableWithoutPublishingAPartialInventory)
{
    client
        ->deploy(
            {.service = "observed",
                .kind = node::proto::MARKET_DATA,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::MARKET_DATA),
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
            client =
                AgentClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
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
    EXPECT_EQ(
        health.at("failure").at("message"),
        "Agent initialization failed; inspect node logs: unfinished service configuration " "requires explicit recovery");
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
