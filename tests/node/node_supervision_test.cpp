#include <asterion/protocol/task_execution.hpp>
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "data/history_fixture.hpp"
#include "tasks/paper_input.hpp"
#include "support/task_wait.hpp"
#include "task_store.hpp"
#include "research_data.hpp"
#include <asterion/research.pb.h>
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
#include "support/agent_installation.hpp"
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
        install_node_configuration(root, fs::path(ASTERION_AGENT_PATH).parent_path() / "config");
        endpoint = (root / "agent.sock").string();
        launch();
    }

    void launch()
    {
        agent = std::make_unique<ChildProcess>(
            testing_support::install_agent(root),
            std::vector<std::string>{"--endpoint", endpoint});
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
        if (HasFailure()) {
            std::cerr << "Retained isolated Node supervision evidence: " << root << '\n';
            return;
        }
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
        const auto kind = task ? node::proto::RESEARCH : node::proto::MARKET;
        client
            ->deploy(
                {.service = name,
                    .kind = kind,
                    .platform = current_platform(),
                    .programs = local_service_programs(kind),
                    .plugins = std::vector<PluginArtifact>{}})
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
    const auto directory = root / "agent" / "data";
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
        research::proto::ResearchRequest request;
        ASSERT_TRUE(request.ParseFromString(held.receive(1s)));
        ASSERT_TRUE(request.has_heartbeat());
        task::proto::TaskResponse reply;
        reply.set_version(1);
        reply.set_service_id(request.service_id());
        reply.set_correlation_id(request.correlation_id());
        reply.mutable_health()->set_instance_id(request.service_id());
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

TEST_F(NodeSupervision, DataInstanceRunsIndependentlyWithoutReverseBinding)
{
    client
        ->deploy(
            {.service = "data-one",
                .kind = node::proto::DATA,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::DATA),
                .plugins = std::vector<PluginArtifact>{}})
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
    EXPECT_FALSE(first.contains("task_service"));
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
    EXPECT_FALSE(state("data-one").contains("task_service"));
}

TEST_F(NodeSupervision, ResearchStartsWithoutDataAndKeepsIndependentOwnershipAcrossRestart)
{
    const auto deploy = [&](const std::string& name, node::proto::ServiceKind kind) {
        client
            ->deploy(
                {.service = name,
                    .kind = kind,
                    .platform = current_platform(),
                    .programs = local_service_programs(kind),
                    .plugins = std::vector<PluginArtifact>{}})
            .get();
    };
    const auto ready = [&](const std::string& name) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            const auto service = state(name);
            if (service.at("health") == "ready")
                return service;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error(service.dump());
            std::this_thread::sleep_for(20ms);
        }
    };
    deploy("tasks-one", node::proto::RESEARCH);
    const auto first = ready("tasks-one");
    EXPECT_FALSE(first.contains("data_service"));
    EXPECT_EQ(health().at("services").size(), 1U);
    auto endpoint = client->service_endpoint("tasks-one", node::proto::RESEARCH).get();
    EXPECT_FALSE(endpoint.owner_node_id.empty());
    const auto call = [&](research::proto::ResearchRequest request) {
        request.set_version(1);
        request.set_service_id("tasks-one");
        request.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(endpoint.endpoint, 2s);
        channel.send(request.SerializeAsString(), 2s);
        task::proto::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(2s)) || reply.has_error())
            throw std::runtime_error("research fixture request failed");
        return reply;
    };
    research::proto::ResearchRequest heartbeat;
    heartbeat.mutable_heartbeat();
    const auto before = call(heartbeat).health();
    EXPECT_EQ(before.instance_id(), "tasks-one");
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(200);
    EXPECT_EQ(call(list).tasks().tasks_size(), 0);
    client->action("tasks-one", "restart").get();
    (void)ready("tasks-one");
    const auto after = call(heartbeat).health();
    EXPECT_EQ(after.instance_id(), before.instance_id());
    EXPECT_NE(after.process_id(), before.process_id());
    deploy("research-two", node::proto::RESEARCH);
    (void)ready("research-two");
    EXPECT_EQ(health().at("services").size(), 2U);
    for (const auto* id : {"data-one", "data-two"}) {
        deploy(id, node::proto::DATA);
        (void)ready(id);
        client->action(id, "stop").get();
    }
    // Discovery candidates do not become deployment dependencies.
    client->action("tasks-one", "restart").get();
    (void)ready("tasks-one");
    EXPECT_EQ(call(list).tasks().tasks_size(), 0);
    restart();
    (void)ready("tasks-one");
    const auto restored = client->service_endpoint("tasks-one", node::proto::RESEARCH).get();
    EXPECT_EQ(restored.owner_node_id, endpoint.owner_node_id);
    EXPECT_EQ(restored.session, endpoint.session);
    EXPECT_NE(restored.endpoint, endpoint.endpoint);
    endpoint = restored; // A locator must be discovered again after Agent restart.
    EXPECT_EQ(call(list).tasks().tasks_size(), 0);
}

TEST_F(NodeSupervision, SlowProcessReleaseKeepsStatusAndConfirmedStopIntentVisible)
{
    client
        ->deploy(
            {.service = "observed",
                .kind = node::proto::MARKET,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::MARKET),
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
                .kind = node::proto::MARKET,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::MARKET),
                .plugins = std::vector<PluginArtifact>{}})
        .get();
    const auto before = state();
    const auto config = root / "agent/data/services/observed/service.json";
    const auto pending = root / "agent/data/services/observed/service.pending";
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
                .kind = node::proto::MARKET,
                .platform = current_platform(),
                .programs = local_service_programs(node::proto::MARKET),
                .plugins = std::vector<PluginArtifact>{}})
        .get();
    client.reset();
    agent.reset();
    const auto config = root / "agent/data/services/observed/service.json";
    const auto digest = sha256_file(config);
    // A separate incomplete service must prevent publishing the otherwise valid inventory.
    fs::create_directories(root / "agent/data/services/incomplete");
    const auto pending = root / "agent/data/services/incomplete/service.pending";
    write_file_durably(pending, "test-owned unfinished publication");
    agent = std::make_unique<ChildProcess>(
        testing_support::install_agent(root),
        std::vector<std::string>{"--endpoint", endpoint});
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
    // The refusal names the configuration it could not read.
    EXPECT_EQ(
        health.at("failure").at("message"),
        "Agent initialization failed; inspect node logs: "
            + (root / "agent/data/services/incomplete/service.json").string()
            + ": unfinished service configuration requires explicit recovery");
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

TEST_F(NodeSupervision, DataQueriesDurableResearchReleaseAndKeepsUnknownOrAcceptedReferences)
{
    const auto deploy = [&](const std::string& name, node::proto::ServiceKind kind) {
        client
            ->deploy(
                {.service = name,
                    .kind = kind,
                    .platform = current_platform(),
                    .programs = local_service_programs(kind),
                    .plugins = kind == node::proto::RESEARCH
                        ? std::nullopt
                        : std::optional{std::vector<PluginArtifact>{}}})
            .get();
    };
    const auto ready = [&](const std::string& name) {
        const auto until = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            const auto observed = state(name);
            if (observed.at("health") == "ready")
                return observed;
            if (std::chrono::steady_clock::now() >= until)
                throw std::runtime_error(observed.dump());
            std::this_thread::sleep_for(20ms);
        }
    };
    deploy("data-one", node::proto::DATA);
    const auto data_root = fs::path(ready("data-one").at("directory").get<std::string>());
    client->action("data-one", "stop").get();
    const auto selected = test::seed_history(
        data_root,
        {100, 101, 102, 101, 100, 101, 103},
        "retained-source",
        {"2026-09-25"},
        {},
        "rb",
        1,
        110,
        "test.confirmed.v1",
        "2026-10",
        100,
        "data-one");
    client->action("data-one", "start").get();
    (void)ready("data-one");
    auto data_address = client->service_endpoint("data-one", node::proto::DATA).get();
    protocol::DataClient data(data_address.endpoint, "data-one");
    data::proto::DataRequest dataset;
    auto* selection = dataset.mutable_bar_dataset();
    selection->set_contract("SHFE/rb/2026-10");
    *selection->mutable_daily() = protocol::encode_daily_table(selected.at("daily"));
    for (const auto& version : selected.at("source_dataset_ids"))
        selection->add_source_dataset_ids(version.get<std::string>());
    auto input = testing_support::backtest_input(data.call(dataset).bar_dataset());
    input.mutable_data_owner()->set_node_id(data_address.owner_node_id);
    input.mutable_data_owner()->set_service_id("data-one");
    deploy("research-one", node::proto::RESEARCH);
    const auto research_root = fs::path(ready("research-one").at("directory").get<std::string>());
    client->action("research-one", "stop").get();
    data::proto::DataRetention retention;
    {
        tasks::Store store(asterion::research::task_domain(), research_root, {"research-one"});
        auto draft = task_submission(store, "pending-release", input);
        store.admit_submission(draft);
        draft.prepare_files();
        tasks::ResearchDataRoute route{
            input.data_owner(),
            data_address.endpoint,
            data_address.owner_node_id
        };
        retention =
            tasks::research_retention(draft.execution(), route, "research-one", "pending-release");
        tasks::save_research_retention(research_root / "pending-release", retention);
        data::proto::DataRequest prepare;
        *prepare.mutable_retention()->mutable_prepare() = retention;
        EXPECT_EQ(
            data.call(prepare).retention().phase(),
            data::proto::DataRetentionRecord::PREPARED);
    }
    client->action("research-one", "start").get();
    (void)ready("research-one");
    auto research_address = client->service_endpoint("research-one", node::proto::RESEARCH).get();
    const auto research = [&](research::proto::ResearchRequest request) {
        request.set_version(1);
        request.set_service_id("research-one");
        request.set_correlation_id(unique_process_id());
        auto peer = ipc::Channel::connect(research_address.endpoint, 2s);
        peer.send(request.SerializeAsString(), 2s);
        task::proto::TaskResponse response;
        if (!response.ParseFromString(peer.receive(10s)))
            throw std::runtime_error("invalid Research fixture response");
        protocol::validate_message(response);
        if (response.has_error())
            throw_remote_error(response.error().code(), response.error().message());
        return response;
    };
    data::proto::DataRequest release;
    *release.mutable_retention()->mutable_release() = retention.consumer();
    EXPECT_THROW(
        data.call(release, 10s),
        Error); // Absence of an admitted task is not a release decision.
    client->action("research-one", "stop").get();
    EXPECT_THROW(data.call(release, 10s), Error); // Lost contact keeps the source held.
    data::proto::DataRequest get;
    *get.mutable_retention()->mutable_get() = retention.consumer();
    EXPECT_EQ(data.call(get).retention().phase(), data::proto::DataRetentionRecord::PREPARED);
    client->action("research-one", "start").get();
    (void)ready("research-one");
    research::proto::ResearchRequest abandon;
    abandon.mutable_abandon_preparation()->set_id("pending-release");
    EXPECT_EQ(
        research(abandon).release_decision().state(),
        resource::proto::ReleaseDecision::RELEASED);
    EXPECT_EQ(
        data.call(release, 10s).retention().phase(),
        data::proto::DataRetentionRecord::RELEASED);
    EXPECT_EQ(
        data.call(release, 10s).retention().phase(),
        data::proto::DataRetentionRecord::RELEASED);
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("pending-release");
    EXPECT_THROW(research(recover), Error);
    client->action("research-one", "stop").get();
    fs::remove_all(
        research_root
        / "pending-release"); // Only this isolated draft payload, never its SQLite decision.
    client->action("research-one", "start").get();
    (void)ready("research-one");
    EXPECT_EQ(
        data.call(release, 10s).retention().phase(),
        data::proto::DataRetentionRecord::RELEASED);
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("accepted-keep");
    *submit.mutable_submit()->mutable_data_owner() = input.data_owner();
    task::proto::Task fixed;
    *fixed.mutable_input() = input;
    *submit.mutable_submit()->mutable_backtest() = protocol::task_execution(fixed, "").backtest();
    EXPECT_EQ(research(submit).task().state(), task::proto::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(research, "accepted-keep").state(),
        task::proto::SUCCEEDED);
    research::proto::ResearchRequest inspect;
    inspect.mutable_get()->set_id("accepted-keep");
    const auto accepted = research(inspect).task();
    tasks::ResearchDataRoute route{
        accepted.input().data_owner(),
        data_address.endpoint,
        data_address.owner_node_id
    };
    const auto held = tasks::research_retention(
        protocol::task_execution(accepted, sha256_bytes(accepted.input().SerializeAsString())),
        route,
        "research-one",
        "accepted-keep");
    *release.mutable_retention()->mutable_release() = held.consumer();
    EXPECT_THROW(data.call(release, 10s), Error);
    abandon.mutable_abandon_preparation()->set_id("accepted-keep");
    EXPECT_THROW(research(abandon), Error);
    *get.mutable_retention()->mutable_get() = held.consumer();
    EXPECT_EQ(data.call(get).retention().phase(), data::proto::DataRetentionRecord::CONFIRMED);
    data::proto::DataRequest heartbeat;
    heartbeat.mutable_heartbeat();
    EXPECT_FALSE(data.call(heartbeat).health().recovery_required());
    const auto node = data_address.owner_node_id;
    restart();
    (void)ready("data-one");
    (void)ready("research-one");
    data_address = client->service_endpoint("data-one", node::proto::DATA).get();
    EXPECT_EQ(data_address.owner_node_id, node);
    data = protocol::DataClient(data_address.endpoint, "data-one");
    research_address = client->service_endpoint("research-one", node::proto::RESEARCH).get();
    *release.mutable_retention()->mutable_release() = retention.consumer();
    EXPECT_EQ(
        data.call(release, 10s).retention().phase(),
        data::proto::DataRetentionRecord::RELEASED);
    EXPECT_EQ(data.call(get).retention().phase(), data::proto::DataRetentionRecord::CONFIRMED);
    research::proto::ResearchRequest retire;
    retire.mutable_retirement()->set_id("accepted-keep");
    EXPECT_EQ(research(retire).retirement().state(), task::proto::TaskRetirement::AVAILABLE);
    retire.mutable_retirement()->set_execute(true);
    const auto retired = research(retire).retirement();
    ASSERT_EQ(retired.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_TRUE(retired.task().has_retirement());
    EXPECT_TRUE(retired.source_release_confirmed());
    EXPECT_FALSE(retired.has_release_error());
    *release.mutable_retention()->mutable_release() = held.consumer();
    EXPECT_EQ(
        data.call(release, 10s).retention().phase(),
        data::proto::DataRetentionRecord::RELEASED);
    EXPECT_EQ(research(retire).retirement().task().updated_at_ms(), retired.task().updated_at_ms());
    restart();
    (void)ready("data-one");
    (void)ready("research-one");
    data_address = client->service_endpoint("data-one", node::proto::DATA).get();
    data = protocol::DataClient(data_address.endpoint, "data-one");
    research_address = client->service_endpoint("research-one", node::proto::RESEARCH).get();
    EXPECT_EQ(research(retire).retirement().state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(
        data.call(release, 10s).retention().phase(),
        data::proto::DataRetentionRecord::RELEASED);
    submit.mutable_submit()->set_id("offline-retire");
    EXPECT_EQ(research(submit).task().state(), task::proto::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(research, "offline-retire").state(),
        task::proto::SUCCEEDED);
    client->action("data-one", "stop").get();
    retire.mutable_retirement()->set_id("offline-retire");
    const auto offline = research(retire).retirement();
    EXPECT_EQ(offline.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_FALSE(offline.source_release_confirmed());
    EXPECT_TRUE(offline.has_release_error());
    client->action("data-one", "start").get();
    (void)ready("data-one");
    const auto replayed = research(retire).retirement();
    EXPECT_EQ(replayed.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_TRUE(replayed.source_release_confirmed());
    EXPECT_FALSE(replayed.has_release_error());
    EXPECT_EQ(replayed.task().updated_at_ms(), offline.task().updated_at_ms());
}

TEST_F(NodeSupervision, UnknownServiceActionDoesNotCreateAServiceOrChangeDesiredState)
{
    for (const auto* action : {"start", "stop", "restart"}) {
        try {
            client->action("not-deployed", action).get();
            FAIL() << "unknown service action was accepted";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), ErrorCode::not_found);
        }
    }
    EXPECT_TRUE(health().at("services").empty());
    EXPECT_FALSE(fs::exists(root / "agent/data/services/not-deployed"));
}
