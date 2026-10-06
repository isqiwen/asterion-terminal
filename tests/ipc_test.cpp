#include <asterion/foundation/error.hpp>
#include "local_listener.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <latch>
#include <vector>
using namespace asterion;
using namespace std::chrono_literals;
namespace {
struct LocalIpc : testing::Test {
  std::filesystem::path root;
  std::string endpoint;
  void SetUp() override {
    root = std::filesystem::path("/tmp") / ("ast-ipc-" + unique_process_id().substr(0, 12));
    endpoint = (root / "channel").string();
    std::filesystem::create_directory(root);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  std::vector<ipc::Channel> fill() {
    std::vector<ipc::Channel> channels;
    for (unsigned i = 0; i < 16; ++i) {
      try {
        channels.push_back(ipc::Channel::connect(endpoint, 20ms));
      } catch (const Error&) {
        return channels;
      }
    }
    throw std::runtime_error("fixture did not saturate pending IPC connections");
  }
};
} // namespace
TEST_F(LocalIpc, BusyConnectionWaitsForCapacityWithoutResendingPayload) {
  testing_support::LocalListener listener(endpoint);
  auto pending = fill();
  ASSERT_FALSE(pending.empty());
  std::promise<void> started;
  auto future = std::async(std::launch::async, [&] {
    started.set_value();
    return ipc::Channel::connect(endpoint, 2s);
  });
  started.get_future().wait();
  EXPECT_EQ(future.wait_for(80ms), std::future_status::timeout);
  for (std::size_t i = 0; i < pending.size(); ++i)
    listener.accept(2s).close();
  auto client = future.get();
  auto server = listener.accept(2s);
  client.send("one-command", 1s);
  EXPECT_EQ(server.receive(1s), "one-command");
  EXPECT_THROW(server.receive(30ms), Error);
  EXPECT_THROW(listener.accept(30ms), Error);
}
TEST_F(LocalIpc, BusyConnectionRespectsSingleDeadline) {
  testing_support::LocalListener listener(endpoint);
  auto pending = fill();
  ASSERT_FALSE(pending.empty());
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THROW(ipc::Channel::connect(endpoint, 120ms), Error);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, 100ms);
  EXPECT_LT(elapsed, 2s);
  for (std::size_t i = 0; i < pending.size(); ++i)
    listener.accept(1s).close();
  EXPECT_THROW(listener.accept(30ms), Error);
}
TEST_F(LocalIpc, ConcurrentConnectionBurstDeliversEachFrameOnce) {
  testing_support::LocalListener listener(endpoint);
  constexpr unsigned count = 12;
  std::latch ready(count), start(1);
  std::vector<std::future<void>> clients;
  for (unsigned i = 0; i < count; ++i)
    clients.push_back(std::async(std::launch::async, [&, i] {
      ready.count_down();
      start.wait();
      auto channel = ipc::Channel::connect(endpoint, 5s);
      const auto message = std::to_string(i);
      channel.send(message, 5s);
      EXPECT_EQ(channel.receive(5s), message);
    }));
  ready.wait();
  start.count_down();
  std::vector<bool> seen(count, false);
  for (unsigned i = 0; i < count; ++i) {
    auto channel = listener.accept(5s);
    const auto message = channel.receive(5s);
    const auto id = std::stoul(message);
    ASSERT_LT(id, count);
    EXPECT_FALSE(seen.at(id));
    seen.at(id) = true;
    channel.send(message, 5s);
  }
  for (auto& client : clients)
    client.get();
  EXPECT_THROW(listener.accept(30ms), Error);
}
TEST_F(LocalIpc, InvalidAndMissingEndpointsFailWithoutPayload) {
  EXPECT_THROW(ipc::Channel::connect("invalid endpoint", 1s), std::invalid_argument);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_THROW(ipc::Channel::connect(endpoint, 100ms), Error);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}
