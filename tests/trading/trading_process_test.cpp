#include <gtest/gtest.h>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/trading_facts.hpp>
#include "facts_archive.hpp"
#include <functional>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::protocol::proto;

namespace {

struct Host
{
    std::filesystem::path directory, node_directory;
    std::string endpoint, id;
    std::unique_ptr<ChildProcess> process;
    ipc::Channel channel;

    explicit Host(
        std::string session,
        std::function<void(const std::filesystem::path&)> prepare = {})
        : id(std::move(session))
    {
        directory =
            std::filesystem::temp_directory_path() / ("asterion-process-" + unique_process_id());
        std::filesystem::create_directory(directory);
        node_directory = directory.parent_path() / (directory.filename().string() + ".node");
        std::filesystem::create_directory(node_directory);
        if (prepare)
            prepare(directory);
        // macOS's temp_directory_path can exceed sockaddr_un's path length.
        endpoint = "/tmp/ast-" + unique_process_id() + ".sock";
        const auto executable = current_executable().parent_path() / "asterion_trading";
        const auto path = directory.u8string();
        process = std::make_unique<ChildProcess>(
            "/usr/bin/env",
            std::vector<std::string>{
                "ASTERION_NODE_DIRECTORY=" + node_directory.string(),
                "ASTERION_TEST_NODE_ISOLATED=1",
                executable.string(),
                "--node-id",
                "fixture-node",
                "--session",
                id,
                "--endpoint",
                endpoint,
                "--directory",
                std::string(path.begin(), path.end()),
                "--ctp-library",
                ASTERION_TEST_CTP_TRADER
            });
        const auto end = std::chrono::steady_clock::now() + 3s;
        for (;;) {
            try {
                channel = ipc::Channel::connect(endpoint, 200ms);
                break;
            } catch (const Error&) {
                if (process->exited() || std::chrono::steady_clock::now() >= end)
                    throw;
                std::this_thread::sleep_for(10ms);
            }
        }
    }

    ~Host()
    {
        channel.close();
        process.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::filesystem::remove_all(node_directory, ignored);
    }

    wire::Request request()
    {
        wire::Request r;
        r.set_version(1);
        r.set_session_id(id);
        r.set_correlation_id("test.request");
        return r;
    }

    wire::Response call(const wire::Request& r)
    {
        channel.send(r.SerializeAsString(), 2s);
        wire::Response response;
        if (!response.ParseFromString(channel.receive(2s)))
            throw std::runtime_error("bad response");
        EXPECT_EQ(response.session_id(), id);
        EXPECT_EQ(response.correlation_id(), r.correlation_id());
        return response;
    }
};

} // namespace

TEST(TradingProcess, RejectsVersionAndSessionMismatchBeforeWritingARecord)
{
    Host host("live.identity");
    auto r = host.request();
    r.mutable_attach();
    r.set_version(2);
    EXPECT_TRUE(host.call(r).has_error());
    r.set_version(1);
    r.set_session_id("other");
    EXPECT_TRUE(host.call(r).has_error());
    r.set_session_id(host.id);
    EXPECT_TRUE(host.call(r).has_uninitialized());
    // Nothing was created: only the service's own log directory may exist.
    EXPECT_FALSE(std::filesystem::exists(host.directory / "journal.sqlite"));
    r = host.request();
    r.mutable_shutdown();
    EXPECT_TRUE(host.call(r).has_error());
    EXPECT_FALSE(host.process->exited());
}

TEST(TradingProcess, IdleConnectionsDoNotOccupyBusinessExecutionCapacity)
{
    Host host("live.idle");
    std::vector<ipc::Channel> idle;
    for (int i = 0; i < 24; ++i)
        idle.push_back(ipc::Channel::connect(host.endpoint, 1s));
    auto active = ipc::Channel::connect(host.endpoint, 1s);
    auto request = host.request();
    request.mutable_heartbeat();
    active.send(request.SerializeAsString(), 1s);
    wire::Response response;
    ASSERT_TRUE(response.ParseFromString(active.receive(1s)));
    EXPECT_TRUE(response.has_health());
    EXPECT_FALSE(response.health().initialized());
    EXPECT_EQ(response.correlation_id(), request.correlation_id());
}

TEST(TradingProcess, PublishedFactsAreExactReadOnlyAndIndependentOfAccountActivation)
{
    wire::TradingFactsCapture original;
    auto& facts = *original.mutable_facts();
    auto& owner = *facts.mutable_source();
    owner.set_node_id("fixture-node");
    owner.set_service_id("facts.reader");
    owner.set_domain("trading");
    facts.set_account_id("fixture-account");
    facts.set_environment(wire::BROKER_SIMULATION);
    facts.set_instance("original-instance");
    facts.set_id("fixed-one");
    facts.set_captured_at_ms(1790000000000);
    facts.set_policy_revision("fixed-policy");
    facts.set_risk_artifact(std::string(64, 'a'));
    facts.set_phase("disconnected");
    facts.set_broker_sequence(9007199254740993ULL);
    facts.set_connection_generation(9007199254740995ULL);
    auto& reference = *original.mutable_reference();
    *reference.mutable_owner() = owner;
    reference.set_kind("trading_facts");
    reference.set_id(facts.id());
    reference.set_version(sha256_bytes(facts.SerializeAsString()));
    auto unconfirmed = original;
    unconfirmed.mutable_facts()->set_id("not-published");
    unconfirmed.mutable_reference()->set_id("not-published");
    unconfirmed.mutable_reference()->set_version(
        sha256_bytes(unconfirmed.facts().SerializeAsString()));
    wire::TradingFactsReceipt receipt;
    *receipt.mutable_reference() = reference;
    receipt.set_state(wire::TradingFactsReceipt::PUBLISHED);
    receipt.set_admitted_at_ms(1790000000000);
    receipt.set_published_at_ms(1790000000001);
    // Read-side fixture: previously published immutable material; the real
    // account admission/publication/recovery chain is covered by Live.Facts*.
    Host host("facts.reader", [&](const std::filesystem::path& directory) {
        trading::FactsArchive archive(directory, owner, facts.account_id(), facts.environment());
        archive.stage(original);
        archive.publish(reference);
        archive.seal(receipt);
        archive.stage(unconfirmed);
        archive.publish(unconfirmed.reference());
    });
    auto request = host.request();
    *request.mutable_facts()->mutable_read() = reference;
    const auto response = host.call(request);
    ASSERT_TRUE(response.has_facts()) << response.DebugString();
    EXPECT_EQ(response.facts().capture().SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(response.facts().operation().SerializeAsString(), receipt.SerializeAsString());
    EXPECT_EQ(
        protocol::decode_trading_facts_status(response.facts()).at("capture").at("broker_sequence"),
        "9007199254740993");
    *request.mutable_facts()->mutable_read() = unconfirmed.reference();
    const auto pending = host.call(request);
    ASSERT_TRUE(pending.has_error());
    EXPECT_EQ(pending.error().code(), "not_found");
    *request.mutable_facts()->mutable_read() = reference;
    request.mutable_facts()->mutable_read()->mutable_owner()->set_node_id("another-node");
    const auto wrong_owner = host.call(request);
    ASSERT_TRUE(wrong_owner.has_error());
    EXPECT_EQ(wrong_owner.error().code(), "conflict");
    request = host.request();
    request.mutable_facts()->set_capture("must-not-activate-account");
    const auto no_account = host.call(request);
    ASSERT_TRUE(no_account.has_error());
    EXPECT_EQ(no_account.error().code(), "unavailable");
    request = host.request();
    request.mutable_heartbeat();
    const auto health = host.call(request);
    ASSERT_TRUE(health.has_health());
    EXPECT_FALSE(health.health().initialized());
    request = host.request();
    request.mutable_attach();
    EXPECT_TRUE(host.call(request).has_uninitialized());
    EXPECT_FALSE(std::filesystem::exists(host.directory / "journal.sqlite"));
    EXPECT_FALSE(std::filesystem::exists(host.node_directory / "account-owners"));
}
