#include "node_storage_identity.hpp"
#include "agent_client.hpp"
#include "node_paths.hpp"
#include "support/local_listener.hpp"
#include "service_configuration.hpp"
#include <asterion/kernel/process/file_lock.hpp>
#include "support/timing.hpp"
#include <latch>
#include <condition_variable>
#include "node_program.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/environment.hpp>
#include <asterion/node.pb.h>
#include <future>
#include <fstream>
#include <map>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
using namespace asterion::terminal;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

class NodeProgramPublication : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("ast-publication-" + unique_process_id());
    fs::path installed = root / "bin/asterion_node_agent";
    fs::path staged = root / "bin/agent-upgrade.staged";
    fs::path journal = root / "agent-upgrade.json";
    std::string before, after;

    void SetUp() override
    {
        fs::create_directories(root / "bin");
        fs::copy_file(ASTERION_AGENT_PATH, installed);
        before = sha256_file(installed);
        after = sha256_file(ASTERION_MARKET_PATH);
        ASSERT_NE(before, after);
        write_file_durably(
            journal,
            Json{{"installed", installed.string()}, {"before", before}, {"after", after}}.dump());
        fs::copy_file(ASTERION_MARKET_PATH, staged);
        write_file_durably(root / "retained-ledger", "unchanged test-owned business data");
    }

    void TearDown() override
    {
        fail_next_directory_syncs_for_testing(0);
        fs::remove_all(root);
    }

    void resume()
    {
        replace_node_program(ASTERION_MARKET_PATH, installed, root, before);
    }
};

} // namespace

TEST_F(NodeProgramPublication, ConfigurationDeploymentPreservesOperatorEdits)
{
    const auto defaults = fs::path(ASTERION_AGENT_PATH).parent_path() / "config";
    install_node_configuration(root, defaults);
    const auto file = root / "config/research.json";
    std::ifstream input(file);
    EXPECT_EQ(Json::parse(input).at("task_concurrency"), 1);
    const std::string edited = R"({"task_concurrency":2})";
    replace_file_durably(file, edited);
    const auto timestamp = fs::last_write_time(file);
    install_node_configuration(root, defaults);
    std::ifstream preserved(file);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(preserved), {}), edited);
    EXPECT_EQ(fs::last_write_time(file), timestamp);
}

TEST_F(NodeProgramPublication, DirectorySyncFailureKeepsRecoveryIntentUntilAnExplicitRetrySyncs)
{
    const auto retained = sha256_file(root / "retained-ledger");
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(resume(), std::runtime_error);
    ASSERT_EQ(sha256_file(installed), after);
    ASSERT_TRUE(fs::exists(journal));
    ASSERT_FALSE(fs::exists(staged));
    const auto timestamp = fs::last_write_time(installed);
    // The rename is visible, but the first directory synchronization failed.
    // Recovery must retry it before discarding the only durable update intent.
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(resume(), std::runtime_error);
    EXPECT_TRUE(fs::exists(journal));
    fail_next_directory_syncs_for_testing(0);
    EXPECT_NO_THROW(resume());
    EXPECT_FALSE(fs::exists(journal));
    EXPECT_EQ(
        fs::last_write_time(installed).time_since_epoch().count(),
        timestamp.time_since_epoch().count());
    EXPECT_EQ(sha256_file(root / "retained-ledger"), retained);
}

TEST_F(NodeProgramPublication, HardLinkedStagingIsRejectedWithoutReplacingTheInstalledProgram)
{
    fs::create_hard_link(staged, root / "retained-candidate");
    EXPECT_THROW(resume(), std::runtime_error);
    EXPECT_EQ(sha256_file(installed), before);
    EXPECT_EQ(sha256_file(staged), after);
    EXPECT_TRUE(fs::exists(journal));
    EXPECT_EQ(fs::hard_link_count(staged), 2U);
}

TEST_F(NodeProgramPublication, InitialInstallationPublishesCompleteExecutableBeforeAcknowledging)
{
    fs::remove(journal);
    fs::remove(staged);
    fs::remove(installed);
    const auto retained = sha256_file(root / "retained-ledger");
    EXPECT_NO_THROW(install_node_program(ASTERION_AGENT_PATH, installed, root));
    EXPECT_EQ(sha256_file(installed), before);
    EXPECT_TRUE((fs::status(installed).permissions() & fs::perms::owner_exec) != fs::perms::none);
    EXPECT_EQ(std::distance(fs::directory_iterator(root / "bin"), fs::directory_iterator{}), 1);
    const auto timestamp = fs::last_write_time(installed).time_since_epoch().count();
    // Even an identical visible executable is not acknowledged while its
    // installation directory cannot be made durable.
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(install_node_program(ASTERION_AGENT_PATH, installed, root), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_NO_THROW(install_node_program(ASTERION_AGENT_PATH, installed, root));
    EXPECT_EQ(fs::last_write_time(installed).time_since_epoch().count(), timestamp);
    EXPECT_EQ(sha256_file(root / "retained-ledger"), retained);
}

TEST_F(NodeProgramPublication, InitialInstallationPreservesAnExistingDifferentProgram)
{
    fs::remove(journal);
    fs::remove(staged);
    EXPECT_THROW(install_node_program(ASTERION_MARKET_PATH, installed, root), std::runtime_error);
    EXPECT_EQ(sha256_file(installed), before);
    EXPECT_EQ(std::distance(fs::directory_iterator(root / "bin"), fs::directory_iterator{}), 1);
}

TEST(NodeMaintenance, freezes_mutations_and_serializes_start)
{
    ServiceIo io;
    const auto root = fs::temp_directory_path() / ("ast-maint-" + unique_process_id().substr(0, 8));
    fs::create_directories(root / "ledger");
    fs::create_directories(root / "agent");
    install_node_configuration(
        root / "agent",
        fs::path(ASTERION_AGENT_PATH).parent_path() / "config");

    struct Cleanup
    {
        fs::path root;

        ~Cleanup()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } cleanup{root};

    const auto endpoint = (root / "agent.sock").string();
    ChildProcess agent(
        ASTERION_AGENT_PATH,
        {"--directory", (root / "agent").string(), "--endpoint", endpoint});
    NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
    std::shared_ptr<AgentClient> client;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!client) {
        try {
            client = AgentClient::open(io, config).get();
        } catch (const std::exception&) {
            ASSERT_FALSE(agent.exited());
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::sleep_for(50ms);
        }
    }
    const auto instance = client->status().get().at("health").at("instance_id").get<std::string>();
    const auto platform = current_platform();
    client
        ->deploy(
            {.service = "account",
                .kind = node::proto::LIVE_TRADING,
                .platform = platform,
                .programs =
                    {.executable = ASTERION_TRADE_PATH, .catalog = ASTERION_FAKE_CTP_TRADER},
                .directory = (root / "ledger").string()})
        .get();
    EXPECT_THROW(
        (void)client->coordinate_upgrade("upgrade.active-trading", "prepare").get(),
        std::exception);
    EXPECT_EQ(client->status().get().at("state"), "online");
    EXPECT_TRUE(client->status().get().at("error").get<std::string>().empty());
    EXPECT_FALSE(client->status().get().at("health").at("maintenance").get<bool>());
    EXPECT_THROW(client->maintenance(true, "upgrade.test", instance).get(), std::exception);
    client->action("account", "stop").get();
    EXPECT_THROW(client->maintenance(true, "upgrade.test", "different").get(), std::exception);
    client->maintenance(true, "upgrade.test", instance).get();
    client->maintenance(true, "upgrade.test",
              instance).get(); // Exact retry is idempotent.
    EXPECT_THROW(client->maintenance(true, "other", instance).get(), std::exception);
    auto second = AgentClient::open(io, config).get();
    EXPECT_TRUE(second->status().get().at("health").at("maintenance").get<bool>());
    EXPECT_EQ(second->status().get().at("health").at("pid").get<std::uint64_t>(), agent.id());
    EXPECT_THROW(second->action("account", "start").get(), std::exception);
    EXPECT_THROW(second->action("account", "stop").get(), std::exception);
    EXPECT_THROW(second->maintenance(false, "wrong", instance).get(), std::exception);
    EXPECT_EQ(second->status().get().at("state"), "online");
    EXPECT_TRUE(second->status().get().at("error").get<std::string>().empty());
    // Verify every mutation family is rejected before payload handling.
    for (int operation = 0; operation < 8; ++operation) {
        node::proto::Request request;
        request.set_version(1);
        request.set_correlation_id("blocked");
        switch (operation) {
            case 0:
                request.mutable_upload();
                break;
            case 1:
                request.mutable_chunk();
                break;
            case 2:
                request.mutable_finish();
                break;
            case 3:
                request.mutable_deploy();
                break;
            case 4:
                request.mutable_update();
                break;
            case 5:
                request.mutable_firewall();
                break;
            case 6:
                request.mutable_configure_plugins();
                break;
            default:
                request.mutable_action();
                break;
        }
        auto channel = ipc::Channel::connect(endpoint, 3s);
        channel.send(request.SerializeAsString(), 3s);
        node::proto::Response response;
        ASSERT_TRUE(response.ParseFromString(channel.receive(3s)));
        ASSERT_TRUE(response.has_error());
        EXPECT_NE(response.error().message().find("maintenance"), std::string::npos);
    }
    client->maintenance(false, "upgrade.test", instance).get();
    for (int i = 0; i < 4; ++i) {
        auto enter = std::async(std::launch::async, [&] {
            try {
                client->maintenance(true, "race", instance).get();
                return std::string{};
            } catch (const std::exception& error) {
                return std::string(error.what());
            }
        });
        auto start = std::async(std::launch::async, [&] {
            try {
                second->action("account", "start").get();
                return std::string{};
            } catch (const std::exception& error) {
                return std::string(error.what());
            }
        });
        const auto enter_error = enter.get(), start_error = start.get();
        const bool entered = enter_error.empty(), started = start_error.empty();
        ASSERT_NE(entered, started) << "maintenance: " << enter_error << "; start: " << start_error;
        auto observed = AgentClient::open(io, config).get();
        const auto health = observed->status().get().at("health");
        EXPECT_EQ(health.at("maintenance").get<bool>(), entered);
        EXPECT_EQ(health.at("services").at(0).at("desired_running").get<bool>(), started);
        if (entered)
            client->maintenance(false, "race", instance).get();
        else
            second->action("account", "stop").get();
    }
}

TEST(NodeMaintenance, AcknowledgedMutationIsNotReportedFailedWhenStatusReadFails)
{
    ServiceIo io;
    for (const bool maintenance : {true, false}) {
        const auto root =
            fs::temp_directory_path() / ("ast-ack-" + unique_process_id().substr(0, 8));
        fs::create_directory(root);

        struct Cleanup
        {
            fs::path root;

            ~Cleanup()
            {
                std::error_code ec;
                fs::remove_all(root, ec);
            }
        } cleanup{root};

        const auto endpoint = (root / "test.sock").string();
        testing_support::LocalListener listener(endpoint);
        auto server = std::async(std::launch::async, [&] {
            for (int i = 0; i < 3; ++i) {
                auto channel = listener.accept(3s);
                node::proto::Request request;
                if (!request.ParseFromString(channel.receive(3s)))
                    throw std::runtime_error("invalid test request");
                node::proto::Response response;
                response.set_version(1);
                response.set_correlation_id(request.correlation_id());
                if (i == 0) {
                    EXPECT_TRUE(request.has_status());
                    auto* status = response.mutable_status();
                    status->set_phase(asterion::node::proto::Status::READY);
                    status->set_instance_id("test-instance");
                    status->set_os("test-os");
                    status->set_arch("test-arch");
                } else if (i == 1) {
                    EXPECT_TRUE(maintenance ? request.has_maintenance() : request.has_action());
                    response.mutable_accepted();
                } else {
                    EXPECT_TRUE(request.has_status());
                    // The peer has acknowledged the mutation, then the status response is
                    // unavailable. Do not resend the mutation or invent fresh health.
                    channel.close();
                    continue;
                }
                channel.send(response.SerializeAsString(), 3s);
            }
        });
        auto client =
            AgentClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
        if (maintenance)
            EXPECT_NO_THROW(client->maintenance(true, "operation", "test-instance").get());
        else
            EXPECT_NO_THROW(client->action("account", "start").get());
        server.get();
        const auto state = client->status().get();
        EXPECT_EQ(state.at("state"), "unreachable");
        EXPECT_TRUE(state.at("error").get<std::string>().starts_with("Agent status exchange: "));
        EXPECT_EQ(state.at("health").at("instance_id"), "test-instance");
    }
}

TEST(NodeMaintenance, SilentPeerAndBusyAdministrationLeaveStatusAndReadsAvailable)
{
    std::latch release(1);
    std::mutex mutex;
    std::condition_variable entered;
    unsigned active = 0;
    ServiceIo io;

    struct Release
    {
        std::latch& gate;

        ~Release()
        {
            gate.count_down();
        }
    } release_work{release};

    const auto root = fs::temp_directory_path() / ("ast-slow-" + unique_process_id().substr(0, 8));
    fs::create_directory(root);

    struct Cleanup
    {
        fs::path root;

        ~Cleanup()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } cleanup{root};

    const auto endpoint = (root / "agent.sock").string();
    install_node_configuration(root, fs::path(ASTERION_AGENT_PATH).parent_path() / "config");
    ChildProcess agent(ASTERION_AGENT_PATH, {"--directory", root.string(), "--endpoint", endpoint});
    NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        try {
            auto ready = AgentClient::open(io, config).get();
            break;
        } catch (const Error&) {
            ASSERT_FALSE(agent.exited());
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::sleep_for(20ms);
        }
    }
    auto silent = ipc::Channel::connect(endpoint, 1s);
    std::shared_ptr<AgentClient> healthy;
    ASSERT_NO_THROW(healthy = AgentClient::open(io, config).get());
    EXPECT_EQ(healthy->status().get().at("state"), "online");
    EXPECT_EQ(healthy->status().get().at("health").at("pid").get<std::uint64_t>(), agent.id());
    auto work = [&](std::stop_token) -> PolledTask<void> {
        co_await io.admin<void>([&] {
            {
                std::lock_guard lock(mutex);
                ++active;
                entered.notify_one();
            }
            release.wait();
        });
    };
    std::vector<std::future<void>> pending;
    for (unsigned i = 0; i < 2; ++i)
        pending.push_back(io.submit<void>(work));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(entered.wait_for(lock, testing_support::bound(1s), [&] {
            return active == 2;
        }));
    }
    for (unsigned i = 0; i < 8; ++i)
        pending.push_back(io.submit<void>(work));
    auto excess = io.submit<void>(work);
    ASSERT_EQ(excess.wait_for(testing_support::bound(500ms)), std::future_status::ready);
    EXPECT_THROW(excess.get(), Error);
    // Management admission is separate from the ordinary request budget, even
    // while those transactions are suspended rather than running on a worker.
    for (unsigned i = 0; i < 8; ++i)
        pending.push_back(io.submit<void>(
            [&](std::stop_token) -> PolledTask<void> {
                co_await PollUntil{[&] {
                    return release.try_wait();
                }};
            },
            ServiceIo::Lane::administration));
    EXPECT_THROW((void)healthy->action("missing", "stop"), Error);
    auto reading = io.submit<void>([&, healthy](std::stop_token) -> PolledTask<void> {
        // A real status exchange and a read-pool job both complete on this owner
        // while every management worker and queue position is occupied.
        const auto inventory = co_await PollFuture{healthy->history_inventory()};
        EXPECT_TRUE(inventory.empty());
        const auto status = co_await PollFuture{healthy->status()};
        EXPECT_EQ(status.at("state"), "online");
        const auto result = co_await io.read<int>([] {
            return 42;
        });
        EXPECT_EQ(result, 42);
    });
    ASSERT_EQ(reading.wait_for(testing_support::bound(1s)), std::future_status::ready);
    EXPECT_NO_THROW(reading.get());
}

TEST(NodeMaintenance, UpgradePreservesDesiredStateAcrossAgentRestartAndCompletionRetry)
{
    ServiceIo io;
    const auto root =
        fs::temp_directory_path() / ("ast-upgrade-" + unique_process_id().substr(0, 8));
    fs::create_directories(root / "agent");
    install_node_configuration(
        root / "agent",
        fs::path(ASTERION_AGENT_PATH).parent_path() / "config");
    fs::create_directories(root / "running");
    fs::create_directories(root / "stopped");

    struct Cleanup
    {
        fs::path root;

        ~Cleanup()
        {
            std::error_code e;
            fs::remove_all(root, e);
        }
    } cleanup{root};

    const auto endpoint = (root / "node.sock").string();
    const NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
    std::unique_ptr<ChildProcess> agent;
    std::shared_ptr<AgentClient> client;
    auto start = [&] {
        fs::remove(endpoint);
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
                client = AgentClient::open(io, config).get();
                break;
            } catch (const std::exception&) {
                if (agent->exited() || std::chrono::steady_clock::now() > deadline)
                    throw;
                std::this_thread::sleep_for(20ms);
            }
        }
    };
    auto wait_phase = [&](const std::string& action, const std::string& phase) {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        for (;;) {
            try {
                if (client->coordinate_upgrade("upgrade.test", action).get().at("phase") == phase)
                    break;
            } catch (const std::exception&) {
                if (std::chrono::steady_clock::now() > deadline)
                    throw;
            }
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::sleep_for(100ms);
        }
    };
    start();
    const auto platform = current_platform();
    for (const auto* name : {"running", "stopped"})
        client
            ->deploy(
                {.service = name,
                    .kind = node::proto::MARKET_DATA,
                    .platform = platform,
                    .programs =
                        {.executable = ASTERION_MARKET_PATH, .provider = ASTERION_FAKE_CTP}})
            .get();
    client->action("stopped", "stop").get();
    EXPECT_EQ(client->coordinate_upgrade("upgrade.test", "prepare").get().at("phase"), "draining");
    // Lose the coordinator while draining: persisted process identities fence restart.
    client.reset();
    agent.reset();
    start();
    wait_phase("prepare", "ready");
    EXPECT_THROW(client->action("stopped", "start").get(), std::exception);
    EXPECT_THROW(
        (void)client->coordinate_upgrade("upgrade.other", "prepare").get(),
        std::exception);
    client.reset();
    agent.reset();
    start();
    const auto frozen = client->status().get();
    for (const auto& service : frozen.at("health").at("services"))
        EXPECT_EQ(service.at("pid"), 0);
    wait_phase("resume", "restoring");
    wait_phase("complete", "complete");
    EXPECT_EQ(client->coordinate_upgrade("upgrade.test", "resume").get().at("phase"), "complete");
    EXPECT_EQ(client->coordinate_upgrade("upgrade.test", "complete").get().at("phase"), "complete");
    EXPECT_THROW((void)client->coordinate_upgrade("upgrade.test", "unknown").get(), std::exception);
    client.reset();
    agent.reset();
    start();
    // Restart restores desired configurations first; process preparation runs
    // asynchronously while Agent status is already available.
    const auto restored = std::chrono::steady_clock::now() + 10s;
    Json status;
    for (;;) {
        (void)client->history_inventory().get();
        status = client->status().get();
        bool prepared = true;
        for (const auto& service : status.at("health").at("services"))
            if (service.at("desired_running") && service.at("pid") == 0)
                prepared = false;
        if (prepared)
            break;
        ASSERT_LT(std::chrono::steady_clock::now(), restored);
        std::this_thread::sleep_for(20ms);
    }
    for (const auto& service : status.at("health").at("services")) {
        const bool running = service.at("id") == "running";
        EXPECT_EQ(service.at("desired_running"), running);
        EXPECT_EQ(service.at("pid").get<std::uint64_t>() != 0, running);
    }
    client.reset();
    agent.reset();
}

namespace {

class ProgramPreparation : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("ast-dev-programs-" + unique_process_id());
    node::proto::ServicePrograms manifest;

    void SetUp() override
    {
        fs::create_directories(root / "services/market/ledger");
        fs::create_directories(root / "services/trading/ledger");
        fs::create_directories(root / "artifacts");
        const auto old = sha256_file(ASTERION_AGENT_PATH);
        fs::copy_file(ASTERION_AGENT_PATH, root / "artifacts" / (old + ".bin"));
        for (const auto& name : {"market", "trading"}) {
            agent::ServiceConfiguration config;
            config.kind = std::string(name) == "market" ? node::proto::MARKET_DATA
                                                        : node::proto::LIVE_TRADING;
            config.artifact = old;
            config.catalog_artifact = old;
            config.provider_artifact = config.kind == node::proto::MARKET_DATA ? old : "";
            config.desired = config.kind == node::proto::MARKET_DATA;
            config.directory = (root / "services" / name / "ledger").string();
            agent::save_service_configuration(root / "services" / name, config);
            write_file_durably(fs::path(config.directory) / "retained", "business data");
            auto* source = manifest.add_services();
            source->set_kind(config.kind);
            source->set_executable(config.desired ? ASTERION_MARKET_PATH : ASTERION_TRADE_PATH);
            source->set_catalog(ASTERION_FAKE_CTP_TRADER);
            if (config.desired)
                source->set_provider(ASTERION_FAKE_CTP);
        }
        manifest.set_version(1);
        manifest.add_bundled_plugins(PLUGIN_NEWER);
        auto selected = agent::load_service_configuration(root / "services/market", true, 0);
        for (const auto* plugin : {PLUGIN_GOOD, PLUGIN_CUSTOM}) {
            const auto digest = sha256_file(plugin);
            fs::copy_file(plugin, root / "artifacts" / (digest + ".bin"));
            selected.plugin_artifacts.push_back(digest);
        }
        agent::save_service_configuration(root / "services/market", selected);
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    fs::path copy(const std::string& hash) const
    {
        return root / "artifacts" / (hash + ".bin");
    }

    int prepare()
    {
        replace_file_durably(root / "programs.pb", manifest.SerializeAsString());
        // Only this child receives the development environment.
        ChildProcess child(
            "/usr/bin/env",
            {"ASTERION_ENVIRONMENT=development",
                ASTERION_AGENT_PATH,
                "--directory",
                root.string(),
                "--prepare-programs",
                (root / "programs.pb").string()},
            false,
            root / ("preparation-" + unique_process_id() + ".log"),
            true);
        if (!child.wait(15s))
            throw std::runtime_error("preparation timed out");
        return child.exit_code();
    }
};

} // namespace

TEST_F(ProgramPreparation, SynchronizesBeforeStartupAndPreservesIntentAndLedgers)
{
    // What an earlier start loaded the market service's plugins from.
    fs::create_directories(root / "services/market/plugins/earlier");
    write_file_durably(root / "services/market/plugins/earlier/plugin.so", "a copy");
    ASSERT_EQ(prepare(), 0);
    const auto market = agent::load_service_configuration(root / "services/market", true, 0);
    const auto trading = agent::load_service_configuration(root / "services/trading", true, 0);
    EXPECT_EQ(
        market.plugin_artifacts,
        (std::vector<std::string>{sha256_file(PLUGIN_NEWER), sha256_file(PLUGIN_CUSTOM)}));
    EXPECT_TRUE(market.desired);
    EXPECT_FALSE(trading.desired);
    EXPECT_EQ(market.artifact, sha256_file(ASTERION_MARKET_PATH));
    EXPECT_EQ(market.provider_artifact, sha256_file(ASTERION_FAKE_CTP));
    EXPECT_EQ(market.catalog_artifact, sha256_file(ASTERION_FAKE_CTP_TRADER));
    EXPECT_EQ(trading.artifact, sha256_file(ASTERION_TRADE_PATH));
    for (const auto& config : {market, trading}) {
        EXPECT_EQ(
            sha256_file(fs::path(config.directory) / "retained"),
            sha256_bytes("business data"));
        EXPECT_EQ(config.port, 0);
    }
    // The program of the build before runs in no service any more: its copy is
    // gone, or every build would leave one. A plugin that was replaced stays,
    // for a task submitted under it; the folder plugins were loaded from is a
    // copy of artifacts and goes.
    EXPECT_FALSE(fs::exists(copy(sha256_file(ASTERION_AGENT_PATH))));
    EXPECT_TRUE(fs::exists(copy(sha256_file(PLUGIN_GOOD))));
    EXPECT_FALSE(fs::exists(root / "services/market/plugins"));
    const auto modified = fs::last_write_time(root / "services/market/service.json");
    ASSERT_EQ(prepare(), 0);
    EXPECT_TRUE(fs::last_write_time(root / "services/market/service.json") == modified);
    EXPECT_FALSE(fs::exists(root / "agent.pid"));
    // A start that changes nothing removes nothing.
    for (const auto& hash :
        {market.artifact, market.provider_artifact, market.catalog_artifact, trading.artifact})
        EXPECT_TRUE(fs::exists(copy(hash))) << hash;
}

// A kind of service this node does not have is not staged: one deployed later
// brings its own programs, and a copy nothing runs would stay behind.
TEST_F(ProgramPreparation, StagesOnlyWhatAServiceHereRuns)
{
    fs::remove_all(root / "services/trading");
    ASSERT_EQ(prepare(), 0);
    EXPECT_TRUE(fs::exists(copy(sha256_file(ASTERION_MARKET_PATH))));
    EXPECT_FALSE(fs::exists(copy(sha256_file(ASTERION_TRADE_PATH))));
}

TEST_F(ProgramPreparation, RejectsRunningAgentAndInvalidSelectionWithoutChangingConfiguration)
{
    const auto before = sha256_file(root / "services/market/service.json");
    {
        FileLock running(root, "agent.lock");
        EXPECT_NE(prepare(), 0);
    }
    manifest.mutable_services(1)->clear_catalog();
    EXPECT_NE(prepare(), 0);
    EXPECT_EQ(sha256_file(root / "services/market/service.json"), before);
    EXPECT_FALSE(fs::exists(root / "agent.pid"));
}

TEST_F(ProgramPreparation, TerminalReportsTheFailedPreparationCauseAndExactLog)
{
    struct Environment
    {
        std::map<std::string, std::optional<std::string>> saved;

        void set(const char* name, const std::string& value)
        {
            saved.emplace(name, environment_variable(name));
            ::setenv(name, value.c_str(), 1);
        }

        ~Environment()
        {
            for (const auto& [name, value] : saved) {
                if (value)
                    ::setenv(name.c_str(), value->c_str(), 1);
                else
                    ::unsetenv(name.c_str());
            }
        }
    } environment;

    environment.set("ASTERION_ENVIRONMENT", "development");
    environment.set("ASTERION_NODE_DIRECTORY", root.string());
    fs::create_directory(root / "bundled-plugins");
    environment.set("ASTERION_PLUGIN_DIRECTORY", (root / "bundled-plugins").string());
    const auto invalid = Json{{"kind", static_cast<int>(node::proto::MARKET_DATA)}}.dump();
    replace_file_durably(root / "services/market/service.json", invalid);
    try {
        prepare_local_programs(root, ASTERION_AGENT_PATH);
        FAIL() << "invalid service configuration must reject synchronization";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::operation_failed);
        const std::string diagnostic = error.what();
        EXPECT_TRUE(diagnostic.starts_with("service program synchronization failed: "));
        EXPECT_NE(diagnostic.find("invalid managed service configuration"), std::string::npos);
        const auto marker = diagnostic.find("\nLog file: ");
        ASSERT_NE(marker, std::string::npos);
        const fs::path log = diagnostic.substr(marker + std::string("\nLog file: ").size());
        EXPECT_EQ(log.parent_path(), root / "logs");
        EXPECT_TRUE(log.filename().string().starts_with("agent-"));
        std::ifstream input(log);
        const std::string contents{std::istreambuf_iterator<char>(input), {}};
        EXPECT_NE(contents.find("invalid managed service configuration"), std::string::npos);
    }
    EXPECT_FALSE(fs::exists(root / "agent.pid"));
    std::ifstream input(root / "services/market/service.json");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), invalid);
}

namespace {

struct NodeIdentityDirectory
{
    fs::path root = fs::temp_directory_path() / ("ast-node-identity-" + unique_process_id());

    NodeIdentityDirectory()
    {
        fs::create_directory(root);
    }

    ~NodeIdentityDirectory()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
};

} // namespace

TEST(NodeStorageIdentity, PersistsAcrossProcessGenerationsAndCompletesUncertainCreation)
{
    NodeIdentityDirectory directory;
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(agent::node_storage_identity(directory.root), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    const auto original = agent::node_storage_identity(directory.root);
    EXPECT_FALSE(original.empty());
    EXPECT_NE(original, unique_process_id());
    fs::create_directory(directory.root / "services");
    fs::create_directory(directory.root / "services" / "warehouse");
    EXPECT_EQ(agent::node_storage_identity(directory.root), original);
    EXPECT_EQ(agent::node_storage_identity(directory.root), original);
}

TEST(NodeStorageIdentity, MissingOrInvalidIdentityCannotReidentifyExistingData)
{
    NodeIdentityDirectory directory;
    fs::create_directories(directory.root / "services" / "warehouse");
    const auto evidence = directory.root / "services" / "warehouse" / "evidence";
    write_file_durably(evidence, "preserve this fixture");
    EXPECT_THROW(agent::node_storage_identity(directory.root), std::invalid_argument);
    EXPECT_FALSE(fs::exists(directory.root / "node.json"));
    write_file_durably(
        directory.root / "node.json",
        "{\"node_id\":\"original\",\"unexpected\":true}");
    EXPECT_THROW(agent::node_storage_identity(directory.root), std::invalid_argument);
    std::ifstream stream(evidence);
    std::string value;
    std::getline(stream, value);
    EXPECT_EQ(value, "preserve this fixture");
    std::ifstream identity(directory.root / "node.json");
    std::getline(identity, value);
    EXPECT_EQ(value, "{\"node_id\":\"original\",\"unexpected\":true}");
}

TEST(NodeStorageIdentity, DiscoveryKeepsNodeIdentityAndRefusesAdministrativeRequests)
{
    NodeIdentityDirectory directory;
    const auto endpoint = (directory.root / "agent.sock").string();
    const auto start = [&] {
        return std::make_unique<ChildProcess>(
            ASTERION_AGENT_PATH,
            std::vector<std::string>{
                "--directory",
                directory.root.string(),
                "--endpoint",
                endpoint
            });
    };
    auto process = start();
    const auto query = [&](const std::string& address, bool mutation = false) {
        node::proto::Request request;
        request.set_version(1);
        request.set_correlation_id(unique_process_id());
        if (mutation)
            request.mutable_deploy()->set_service_id("must-not-be-created");
        else
            request.mutable_status();
        auto channel = ipc::Channel::connect(address, 200ms);
        channel.send(request.SerializeAsString(), 2s);
        node::proto::Response response;
        if (!response.ParseFromString(channel.receive(2s)))
            throw std::runtime_error("invalid fixture reply");
        EXPECT_EQ(response.correlation_id(), request.correlation_id());
        return response;
    };
    const auto ready = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                const auto response = query(endpoint);
                if (response.has_error())
                    throw std::runtime_error(response.error().message());
                if (response.status().phase() == node::proto::Status::READY)
                    return response.status();
            } catch (const Error&) {
                if (process->exited() || std::chrono::steady_clock::now() >= deadline)
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("isolated Agent did not become ready");
            std::this_thread::sleep_for(10ms);
        }
    };
    const auto original = ready();
    ASSERT_FALSE(original.node_id().empty());
    ASSERT_FALSE(original.discovery_endpoint().empty());
    EXPECT_EQ(query(original.discovery_endpoint()).status().node_id(), original.node_id());
    const auto denied = query(original.discovery_endpoint(), true);
    ASSERT_TRUE(denied.has_error());
    EXPECT_EQ(denied.error().code(), "permission_denied");
    EXPECT_TRUE(fs::is_empty(directory.root / "services"));
    process.reset();
    process = start();
    const auto restarted = ready();
    EXPECT_EQ(restarted.node_id(), original.node_id());
    EXPECT_NE(restarted.instance_id(), original.instance_id());
    EXPECT_NE(restarted.discovery_endpoint(), original.discovery_endpoint());
    EXPECT_EQ(query(restarted.discovery_endpoint()).status().node_id(), original.node_id());
}
