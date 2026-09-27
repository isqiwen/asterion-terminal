#include <gtest/gtest.h>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <future>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::protocol::v1;
namespace {
struct Endpoint {
  std::string path;
  std::filesystem::path directory;
  Endpoint() {
#ifdef _WIN32
    path = "asterion.test." + unique_process_id();
#else
    directory = std::filesystem::path("/tmp") / ("ast-test-" + unique_process_id());
    std::filesystem::create_directory(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    path = (directory / "socket").string();
#endif
  }
  ~Endpoint() {
    if (!directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove(directory, ignored);
    }
  }
};
} // namespace
TEST(Protobuf, TypedDecimalAndCommandRoundTrip) {
  Json command{{"request_id", "r1"},
               {"action", "submit"},
               {"order_id", "o1"},
               {"side", "sell"},
               {"offset", "close_today"},
               {"quantity", "2"},
               {"price", "92233720368.54775807"}};
  auto message = protocol::encode_command(command);
  EXPECT_EQ(message.submit().price().units(), INT64_MAX);
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
TEST(LocalIpc, BoundedBinaryFramesAndDisconnect) {
  Endpoint endpoint;
  ipc::Listener listener(endpoint.path);
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
TEST(LocalIpc, DeadlineClosesTheChannel) {
  Endpoint endpoint;
  ipc::Listener listener(endpoint.path);
  auto client = ipc::Channel::connect(endpoint.path, 2s);
  auto server = listener.accept(2s);
  EXPECT_THROW(client.receive(20ms), Error);
  EXPECT_THROW(client.send("late", 100ms), Error);
  EXPECT_THROW(server.receive(100ms), Error);
}
