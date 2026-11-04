#include <gtest/gtest.h>
#include "support/local_listener.hpp"
#include <asterion/protocol/trading.hpp>
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/system/process/child.hpp>
#include <future>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <fstream>
#include <cstdlib>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = ::trading;

namespace {

struct Endpoint
{
    std::string path;
    std::filesystem::path directory;

    Endpoint()
    {
        directory = std::filesystem::path("/tmp") / ("ast-test-" + unique_process_id());
        std::filesystem::create_directory(directory);
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        path = (directory / "socket").string();
    }

    ~Endpoint()
    {
        if (!directory.empty()) {
            std::error_code ignored;
            std::filesystem::remove(directory, ignored);
        }
    }
};

} // namespace

TEST(Protobuf, TypedDecimalAndCommandRoundTrip)
{
    Json command{
        {"request_id", "r1"},
        {"action", "submit"},
        {"order_id", "o1"},
        {"venue", "SHFE"},
        {"symbol", "rb2610"},
        {"side", "sell"},
        {"offset", "close_today"},
        {"quantity", "2"},
        {"price", "92233720368.54775807"}
    };
    auto message = protocol::encode_command(command);
    EXPECT_EQ(message.submit_order().price().text(), "92233720368.54775807");
    wire::Command parsed;
    ASSERT_TRUE(parsed.ParseFromString(message.SerializeAsString()));
    EXPECT_EQ(protocol::decode_command(parsed), command);
    auto unknown = message.SerializeAsString();
    unknown.append("\xf8\x07\x01", 3);
    ASSERT_TRUE(parsed.ParseFromString(unknown));
    EXPECT_THROW(protocol::validate_message(parsed), std::invalid_argument);
    auto wrong = command;
    wrong["offset"] = "auto";
    EXPECT_THROW(protocol::encode_command(wrong), std::invalid_argument);
    wire::Command absent;
    EXPECT_THROW(protocol::decode_command(absent), std::invalid_argument);
}

TEST(Protobuf, UnknownFieldsInsideOneofAndRepeatedMessagesAreRejected)
{
    wire::Request request;
    auto* price = request.mutable_send_command()->mutable_submit_order()->mutable_price();
    price->GetReflection()->MutableUnknownFields(price)->AddVarint(127, 1);
    EXPECT_THROW(protocol::validate_message(request), std::invalid_argument);
    request.clear_send_command();
    request.mutable_get_account();
    EXPECT_NO_THROW(protocol::validate_message(request));
    wire::Session snapshot;
    snapshot.add_orders()->mutable_price()->set_text("10");
    auto* second = snapshot.add_orders()->mutable_lots();
    second->GetReflection()->MutableUnknownFields(second)->AddVarint(127, 1);
    EXPECT_THROW(protocol::validate_message(snapshot), std::invalid_argument);
    second->GetReflection()->MutableUnknownFields(second)->Clear();
    EXPECT_NO_THROW(protocol::validate_message(snapshot));
}

TEST(LocalIpc, BoundedBinaryFramesAndDisconnect)
{
    Endpoint endpoint;
    testing_support::LocalListener listener(endpoint.path);
    auto server = std::async(std::launch::async, [&] {
        auto peer = listener.accept(2s);
        auto bytes = peer.receive(2s);
        EXPECT_EQ(bytes, std::string(200000, '\0') + "tail");
        peer.send("ok", 2s);
    });
    auto client = ipc::Channel::connect(endpoint.path, 2s);
    EXPECT_THROW(client.send(std::string(ipc::Channel::max_frame + 1, 'x'), 2s), Error);
    client.send(std::string(200000, '\0') + "tail", 2s);
    EXPECT_EQ(client.receive(2s), "ok");
    server.get();
    EXPECT_THROW(client.receive(100ms), Error);
}

TEST(LocalIpc, DeadlineClosesTheChannel)
{
    Endpoint endpoint;
    testing_support::LocalListener listener(endpoint.path);
    auto client = ipc::Channel::connect(endpoint.path, 2s);
    auto server = listener.accept(2s);
    EXPECT_THROW(client.receive(20ms), Error);
    EXPECT_THROW(client.send("late", 100ms), Error);
    EXPECT_THROW(server.receive(100ms), Error);
}

TEST(Protocol, RpcDiagnosticsNeverSerializeCredentialsOrRemoteErrorMessages)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("asterion-rpc-log-" + unique_process_id());
    std::filesystem::create_directories(root / "config");
    std::ofstream(root / "config" / "logging.json")
        << R"({"console":{"enabled":false,"level":"info"},)" R"("file":{"enabled":true,"level":"info","pattern":"%l %v","path":"../logs/rpc.log","retention_days":1}})";
    log::init(root, "rpc-test");
    wire::Request request;
    request.set_id(7);
    request.mutable_connect_broker();
    wire::Response response;
    response.mutable_error()->set_code(::common::ERROR_CODE_PERMISSION_DENIED);
    response.mutable_error()->set_message("test-only-private-provider-diagnostic");
    protocol::log_rpc_result("rpc-test", request, response, true, {{"session_id", "session.test"}});
    std::vector<std::string> lines;
    for (const auto& entry : std::filesystem::directory_iterator(root / "logs")) {
        std::ifstream stream(entry.path());
        for (std::string line; std::getline(stream, line);)
            lines.push_back(line);
    }
    log::init(root / "none", "test");
    std::filesystem::remove_all(root);
    ASSERT_EQ(lines.size(), 1U);
    // The operation, the error code and the identifiers; never a credential or
    // the remote error's message.
    EXPECT_EQ(
        lines.front(),
        "warning rpc failed: module=rpc-test operation=connect_broker id=7 " "code=permission_denied session_id=session.test");
}
