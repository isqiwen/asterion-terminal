#include "startup.hpp"
#include "support/local_listener.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/manager.pb.h>
#include <gtest/gtest.h>
#include <fstream>
#include <future>
#include <thread>

namespace asterion::installation {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

class InstallationStartup : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("ast-ready-" + unique_process_id());
    std::string socket = (root / "manager.sock").string();

    void SetUp() override
    {
        fs::create_directory(root);
        fs::permissions(root, fs::perms::owner_all);
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    void respond(testing_support::LocalListener& listener, const manager::proto::Snapshot& state)
    {
        auto peer = listener.accept(2s);
        manager::proto::Request request;
        ASSERT_TRUE(request.ParseFromString(peer.receive(2s)));
        ASSERT_TRUE(request.has_snapshot());
        manager::proto::Response response;
        response.set_version(request.version());
        response.set_correlation_id(request.correlation_id());
        *response.mutable_snapshot() = state;
        peer.send(response.SerializeAsString(), 2s);
    }
};

TEST_F(InstallationStartup, WaitsForSocketAndLocalAgentReadiness)
{
    auto server = std::async(std::launch::async, [&] {
        std::this_thread::sleep_for(100ms);
        testing_support::LocalListener listener(socket);
        manager::proto::Snapshot state;
        state.set_pid(100);
        respond(listener, state); // Manager is restoring its registry.
        auto* remote = state.add_nodes();
        remote->set_id("remote");
        remote->set_online(true);
        remote->mutable_health()->set_phase(node::proto::Status::READY);
        auto* local = state.add_nodes();
        local->set_id("local");
        respond(listener, state); // Remote readiness cannot stand in for local readiness.
        local->set_online(true);
        local->mutable_health()->set_phase(node::proto::Status::INITIALIZING);
        respond(listener, state);
        local->mutable_health()->set_phase(node::proto::Status::READY);
        respond(listener, state);
    });
    EXPECT_NO_THROW(wait_for_local_readiness(socket, root, 3s));
    server.get();
}

TEST_F(InstallationStartup, ReportsAgentFailureAndProcessLogs)
{
    testing_support::LocalListener listener(socket);
    auto server = std::async(std::launch::async, [&] {
        manager::proto::Snapshot state;
        state.set_pid(100);
        auto* local = state.add_nodes();
        local->set_id("local");
        local->set_online(true);
        local->mutable_health()->set_pid(200);
        local->mutable_health()->set_phase(node::proto::Status::RECOVERY_REQUIRED);
        local->mutable_health()->mutable_failure()->set_message(
            "cannot read service deployment state");
        respond(listener, state);
    });
    // The Agent's and the Manager's log files, by their installations' settings.
    for (const auto* program : {"agent", "manager"}) {
        std::filesystem::create_directories(root / program / "config");
        std::ofstream(root / program / "config" / "logging.json")
            << R"({"console":{"enabled":false,"level":"info"},)" R"("file":{"enabled":true,"level":"info","path":"../logs/{program}-{pid}.log","retention_days":30}})";
    }
    try {
        wait_for_local_readiness(socket, root, 3s);
        FAIL() << "must reject failed Agent startup";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::recovery_required);
        const std::string message = error.what();
        EXPECT_NE(message.find("cannot read service deployment state"), std::string::npos);
        EXPECT_NE(
            message.find((root / "agent/logs/asterion_node_agent-200_").string()),
            std::string::npos);
        EXPECT_NE(
            message.find((root / "manager/logs/asterion_node_manager-100_").string()),
            std::string::npos);
    }
    server.get();
}

TEST_F(InstallationStartup, MissingManagerTimesOutWithLogDirectory)
{
    const auto start = std::chrono::steady_clock::now();
    try {
        wait_for_local_readiness(socket, root, 150ms);
        FAIL() << "missing Manager must not report success";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::unavailable);
        const std::string message = error.what();
        EXPECT_NE(message.find("management startup timed out: Manager:"), std::string::npos);
        EXPECT_NE(message.find(root.string()), std::string::npos);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
}

TEST_F(InstallationStartup, InvalidResponseFailsWithoutRetry)
{
    testing_support::LocalListener listener(socket);
    auto server = std::async(std::launch::async, [&] {
        auto peer = listener.accept(2s);
        peer.receive(2s);
        peer.send("not a Manager response", 2s);
    });
    try {
        wait_for_local_readiness(socket, root, 3s);
        FAIL() << "invalid protocol must fail";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::invalid_request);
    }
    server.get();
}

} // namespace
} // namespace asterion::installation
